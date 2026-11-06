#pragma once

#include "drpc/drpc.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace drpc {

struct ServerOptions {
  // POSIX only
  std::string directory;

  std::string user_id = "0";
  std::string username = "drpc-server";
};

struct ClientInfo {
  uint64_t id = 0;
  std::string client_id;
  int64_t pid = 0;
};

class Server {
public:
  using ConnectCallback = std::function<void(const ClientInfo &)>;
  using ActivityCallback = std::function<void(const ClientInfo &, const std::optional<Activity> &activity)>;

  explicit Server(ServerOptions options = {});
  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  // callbacks must be set before start() is called.
  void onClientConnected(ConnectCallback cb);
  void onClientDisconnected(ConnectCallback cb);
  void onActivity(ActivityCallback cb);

  bool start();
  void stop();

  std::string endpoint() const;

  std::vector<std::pair<ClientInfo, Activity>> activities() const;

  void pingAll();
  uint64_t pongs() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace drpc
