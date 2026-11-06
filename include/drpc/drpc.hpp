#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace drpc {

struct Button {
  std::string label;
  std::string url;
};

struct Activity {
  std::string details;
  std::string state;

  std::optional<int64_t> start_timestamp;
  std::optional<int64_t> end_timestamp;

  std::string large_image, large_text;
  std::string small_image, small_text;

  std::string party_id;
  int party_size = 0;
  int party_max = 0;

  std::string match_secret, join_secret, spectate_secret;

  std::vector<Button> buttons;
  bool instance = false;

  nlohmann::json toJson() const;
  static Activity fromJson(const nlohmann::json &j);

  // Returns an empty string if the activity looks acceptable to Discord, otherwise a human-readable reason it would be rejected.
  std::string validate() const;
};

enum class Opcode : uint32_t { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

class Client {
public:
  using ReadyCallback = std::function<void(const nlohmann::json &data)>;
  using ErrorCallback = std::function<void(int code, const std::string &message)>;
  using DisconnectCallback = std::function<void(int code, const std::string &message)>;

  explicit Client(std::string client_id);
  ~Client();
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  // callbacks must be set before start() is called.
  void onReady(ReadyCallback cb);
  void onError(ErrorCallback cb);
  void onDisconnected(DisconnectCallback cb);

  void setMinUpdateInterval(std::chrono::milliseconds interval);

  void start();
  void stop();

  void setActivity(const Activity &activity);
  void clearActivity();

  bool connected() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace drpc
