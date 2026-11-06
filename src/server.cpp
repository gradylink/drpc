#include "drpc/server.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace drpc {

using json = nlohmann::json;

namespace {
constexpr int kErrInvalid = 4000;
}

struct Server::Impl {
  struct Session {
    std::unique_ptr<Connection> conn;
    std::thread thread;
    std::atomic<bool> done{false};
    std::mutex writeMutex;
    ClientInfo info;
    bool ready = false;
    std::optional<Activity> activity;
  };

  ServerOptions options;
  Listener listener;
  std::thread acceptThread;
  std::atomic<bool> running{false};
  std::atomic<bool> stopFlag{false};
  std::atomic<uint64_t> nextId{1};
  std::atomic<uint64_t> pongCount{0};

  mutable std::mutex m;
  std::vector<std::unique_ptr<Session>> sessions;

  ConnectCallback connectCb, disconnectCb;
  ActivityCallback activityCb;

  void acceptLoop();
  void serve(Session &s);
  void handleFrame(Session &s, const std::string &payload);
  void reap();

  static bool send(Session &s, Opcode op, const std::string &payload) {
    std::lock_guard<std::mutex> lk(s.writeMutex);
    return writeFrame(*s.conn, op, payload);
  }
  static void reply(Session &s, const json &nonce, const std::string &cmd, const json &data, const char *evt) {
    json msg = {{"cmd", cmd}, {"data", data}, {"evt", evt ? json(evt) : json(nullptr)}, {"nonce", nonce}};
    send(s, Opcode::Frame, dumpJson(msg));
  }
  static void replyError(Session &s, const json &nonce, const std::string &cmd, int code, const std::string &message) {
    reply(s, nonce, cmd, {{"code", code}, {"message", message}}, "ERROR");
  }
};

Server::Server(ServerOptions options) : impl_(new Impl) { impl_->options = std::move(options); }
Server::~Server() { stop(); }

void Server::onClientConnected(ConnectCallback cb) { impl_->connectCb = std::move(cb); }
void Server::onClientDisconnected(ConnectCallback cb) { impl_->disconnectCb = std::move(cb); }
void Server::onActivity(ActivityCallback cb) { impl_->activityCb = std::move(cb); }

bool Server::start() {
  if (impl_->running.load()) return true;
  if (!impl_->listener.listen(impl_->options.directory)) return false;
  impl_->stopFlag = false;
  impl_->running = true;
  impl_->acceptThread = std::thread([this] { impl_->acceptLoop(); });
  return true;
}

void Server::stop() {
  if (!impl_->running.exchange(false)) return;
  impl_->stopFlag = true;
  if (impl_->acceptThread.joinable()) impl_->acceptThread.join();

  std::vector<std::unique_ptr<Impl::Session>> toJoin;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    toJoin.swap(impl_->sessions);
  }
  for (auto &s : toJoin)
    if (s->thread.joinable()) s->thread.join();

  impl_->listener.close();
}

std::string Server::endpoint() const { return impl_->listener.endpoint(); }

std::vector<std::pair<ClientInfo, Activity>> Server::activities() const {
  std::vector<std::pair<ClientInfo, Activity>> out;
  std::lock_guard<std::mutex> lk(impl_->m);
  for (const auto &s : impl_->sessions)
    if (s->ready && s->activity) out.emplace_back(s->info, *s->activity);
  return out;
}

void Server::pingAll() {
  std::lock_guard<std::mutex> lk(impl_->m);
  for (auto &s : impl_->sessions)
    if (s->ready) Impl::send(*s, Opcode::Ping, "{}");
}

uint64_t Server::pongs() const { return impl_->pongCount.load(); }

void Server::Impl::reap() {
  std::lock_guard<std::mutex> lk(m);
  for (auto it = sessions.begin(); it != sessions.end();) {
    if ((*it)->done.load()) {
      if ((*it)->thread.joinable()) (*it)->thread.join();
      it = sessions.erase(it);
    } else {
      ++it;
    }
  }
}

void Server::Impl::acceptLoop() {
  while (!stopFlag) {
    auto conn = listener.accept(200);
    reap();
    if (!conn) continue;

    auto s = std::make_unique<Session>();
    s->conn = std::move(conn);
    s->info.id = nextId++;
    Session *raw = s.get();
    {
      std::lock_guard<std::mutex> lk(m);
      sessions.push_back(std::move(s));
    }
    raw->thread = std::thread([this, raw] {
      serve(*raw);
      raw->conn->close();
      raw->done = true;
    });
  }
}

void Server::Impl::serve(Session &s) {
  Connection &c = *s.conn;
  uint32_t op = 0;
  std::string payload;

  if (c.waitReadable(5000) != 1 || !readFrame(c, op, payload)) return;
  json hs = parseJson(payload);
  bool ok = op == static_cast<uint32_t>(Opcode::Handshake) && hs.is_object() && hs.contains("v") && hs["v"].is_number_integer() && hs["v"].get<int>() == 1 &&
            hs.contains("client_id") && hs["client_id"].is_string() && !hs["client_id"].get<std::string>().empty();
  if (!ok) {
    send(s, Opcode::Close, dumpJson({{"code", 4000}, {"message", "Invalid Client ID"}}));
    return;
  }
  ClientInfo infoCopy;
  {
    std::lock_guard<std::mutex> lk(m);
    s.info.client_id = hs["client_id"].get<std::string>();
    s.ready = true;
    infoCopy = s.info;
  }
  json readyData = {
    {"v", 1},
    {"config", {{"cdn_host", "cdn.discordapp.com"}, {"api_endpoint", "//discord.com/api"}, {"environment", "production"}}},
    {"user", {{"id", options.user_id}, {"username", options.username}, {"discriminator", "0"}}}
  };
  send(s, Opcode::Frame, dumpJson({{"cmd", "DISPATCH"}, {"data", readyData}, {"evt", "READY"}, {"nonce", nullptr}}));
  if (connectCb) connectCb(infoCopy);

  while (!stopFlag) {
    int w = c.waitReadable(200);
    if (w < 0) break;
    if (w == 0) continue;
    if (!readFrame(c, op, payload)) break;

    bool quit = false;
    switch (static_cast<Opcode>(op)) {
    case Opcode::Ping:
      send(s, Opcode::Pong, payload);
      break;
    case Opcode::Pong:
      ++pongCount;
      break;
    case Opcode::Close:
      quit = true;
      break;
    case Opcode::Frame:
      handleFrame(s, payload);
      break;
    default:
      break;
    }
    if (quit) break;
  }

  bool hadActivity;
  {
    std::lock_guard<std::mutex> lk(m);
    s.ready = false;
    hadActivity = s.activity.has_value();
    s.activity.reset();
    infoCopy = s.info;
  }
  if (hadActivity && activityCb) activityCb(infoCopy, std::nullopt);
  if (disconnectCb) disconnectCb(infoCopy);
}

void Server::Impl::handleFrame(Session &s, const std::string &payload) {
  json j = parseJson(payload);
  if (j.is_discarded() || !j.is_object()) {
    replyError(s, nullptr, "", kErrInvalid, "Invalid payload");
    return;
  }
  json nonce = j.contains("nonce") ? j["nonce"] : json(nullptr);
  std::string cmd = (j.contains("cmd") && j["cmd"].is_string()) ? j["cmd"].get<std::string>() : "";

  if (cmd != "SET_ACTIVITY") {
    replyError(s, nonce, cmd, kErrInvalid, "Unknown command");
    return;
  }

  json args = (j.contains("args") && j["args"].is_object()) ? j["args"] : json::object();
  json actJson = args.contains("activity") ? args["activity"] : json(nullptr);

  std::optional<Activity> act;
  if (actJson.is_object()) {
    Activity a = Activity::fromJson(actJson);
    std::string err = a.validate();
    if (!err.empty()) {
      replyError(s, nonce, cmd, kErrInvalid, err);
      return;
    }
    act = std::move(a);
  } else if (!actJson.is_null()) {
    replyError(s, nonce, cmd, kErrInvalid, "activity must be an object or null");
    return;
  }

  ClientInfo infoCopy;
  {
    std::lock_guard<std::mutex> lk(m);
    if (args.contains("pid") && args["pid"].is_number_integer()) s.info.pid = args["pid"].get<int64_t>();
    s.activity = act;
    infoCopy = s.info;
  }

  json data = nullptr;
  if (act) {
    data = actJson;
    data["application_id"] = infoCopy.client_id;
    data["type"] = 0;
  }
  reply(s, nonce, cmd, data, nullptr);
  if (activityCb) activityCb(infoCopy, act);
}

} // namespace drpc
