#include "drpc/drpc.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace drpc {

using json = nlohmann::json;
using steady = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
int64_t currentPid() {
#ifdef _WIN32
  return static_cast<int64_t>(GetCurrentProcessId());
#else
  return static_cast<int64_t>(getpid());
#endif
}
} // namespace

struct Client::Impl {
  std::string clientId;
  std::thread thread;
  std::atomic<bool> stopFlag{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> running{false};

  std::mutex m;
  std::condition_variable cv;
  bool dirty = false;
  bool hasActivity = false;
  Activity activity;
  std::chrono::milliseconds minInterval{15000};
  uint64_t nonce = 0;

  ReadyCallback readyCb;
  ErrorCallback errorCb;
  DisconnectCallback disconnectCb;

  int closeCode = 0;
  std::string closeMessage;

  void run();
  bool session(Connection &c, bool &gotReady);
  bool handleFrame(Connection &c, uint32_t op, const std::string &payload, bool &ready, bool &gotReady);
  void sleepFor(std::chrono::milliseconds d) {
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, d, [&] { return stopFlag.load(); });
  }
};

Client::Client(std::string client_id) : impl_(new Impl) { impl_->clientId = std::move(client_id); }
Client::~Client() { stop(); }

void Client::onReady(ReadyCallback cb) { impl_->readyCb = std::move(cb); }
void Client::onError(ErrorCallback cb) { impl_->errorCb = std::move(cb); }
void Client::onDisconnected(DisconnectCallback cb) { impl_->disconnectCb = std::move(cb); }

void Client::setMinUpdateInterval(std::chrono::milliseconds interval) {
  std::lock_guard<std::mutex> lk(impl_->m);
  impl_->minInterval = interval;
}

void Client::start() {
  if (impl_->running.exchange(true)) return;
  impl_->stopFlag = false;
  impl_->thread = std::thread([this] { impl_->run(); });
}

void Client::stop() {
  if (!impl_->running.load()) return;
  impl_->stopFlag = true;
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->running = false;
  impl_->connected = false;
}

void Client::setActivity(const Activity &a) {
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->activity = a;
    impl_->hasActivity = true;
    impl_->dirty = true;
  }
}

void Client::clearActivity() {
  std::lock_guard<std::mutex> lk(impl_->m);
  impl_->hasActivity = false;
  impl_->dirty = true;
}

bool Client::connected() const { return impl_->connected.load(); }

void Client::Impl::run() {
  std::chrono::milliseconds backoff = 1000ms;
  while (!stopFlag) {
    Connection conn;
    if (!conn.open()) {
      sleepFor(backoff);
      backoff = std::min<std::chrono::milliseconds>(backoff * 2, 30000ms);
      continue;
    }
    closeCode = 0;
    closeMessage = "connection lost";
    bool gotReady = false;
    session(conn, gotReady);
    connected = false;
    conn.close();
    if (stopFlag) break;
    if (disconnectCb) disconnectCb(closeCode, closeMessage);

    if (gotReady) backoff = 1000ms;
    else backoff = std::min<std::chrono::milliseconds>(backoff * 2, 30000ms);
    sleepFor(backoff);
  }
}

bool Client::Impl::session(Connection &c, bool &gotReady) {
  json hs = {{"v", 1}, {"client_id", clientId}};
  if (!writeFrame(c, Opcode::Handshake, dumpJson(hs))) return false;

  bool ready = false;
  bool hasSent = false;
  steady::time_point lastSend;

  while (!stopFlag) {
    if (ready) {
      std::string out;
      {
        std::lock_guard<std::mutex> lk(m);
        if (dirty && (!hasSent || steady::now() - lastSend >= minInterval)) {
          json args = {{"pid", currentPid()}, {"activity", hasActivity ? activity.toJson() : json(nullptr)}};
          json msg = {{"cmd", "SET_ACTIVITY"}, {"args", args}, {"nonce", std::to_string(++nonce)}};
          out = dumpJson(msg);
          dirty = false;
        }
      }
      if (!out.empty()) {
        if (!writeFrame(c, Opcode::Frame, out)) {
          std::lock_guard<std::mutex> lk(m);
          dirty = true; // retry after reconnect
          return false;
        }
        hasSent = true;
        lastSend = steady::now();
      }
    }

    int w = c.waitReadable(200);
    if (w < 0) return false;
    if (w == 0) continue;

    uint32_t op = 0;
    std::string payload;
    if (!readFrame(c, op, payload)) return false;
    if (!handleFrame(c, op, payload, ready, gotReady)) return false;
  }
  return true;
}

bool Client::Impl::handleFrame(Connection &c, uint32_t op, const std::string &payload, bool &ready, bool &gotReady) {
  switch (static_cast<Opcode>(op)) {
  case Opcode::Ping:
    return writeFrame(c, Opcode::Pong, payload);
  case Opcode::Pong:
    return true;
  case Opcode::Close: {
    json j = parseJson(payload);
    if (j.is_object()) {
      if (j.contains("code") && j["code"].is_number()) closeCode = j["code"].get<int>();
      if (j.contains("message") && j["message"].is_string()) closeMessage = j["message"].get<std::string>();
    }
    return false;
  }
  case Opcode::Frame: {
    json j = parseJson(payload);
    if (j.is_discarded() || !j.is_object()) return true;
    std::string evt = (j.contains("evt") && j["evt"].is_string()) ? j["evt"].get<std::string>() : "";
    json data = j.contains("data") ? j["data"] : json(nullptr);
    if (evt == "READY") {
      ready = true;
      gotReady = true;
      connected = true;
      {
        std::lock_guard<std::mutex> lk(m);
        if (hasActivity) dirty = true; // (re)publish after (re)connect
      }
      if (readyCb) readyCb(data);
    } else if (evt == "ERROR") {
      int code = (data.is_object() && data.contains("code") && data["code"].is_number()) ? data["code"].get<int>() : 0;
      std::string msg = (data.is_object() && data.contains("message") && data["message"].is_string()) ? data["message"].get<std::string>() : "";
      if (errorCb) errorCb(code, msg);
    }
    return true;
  }
  default:
    return true;
  }
}

} // namespace drpc
