#include "drpc/server.hpp"

#include <iostream>
#include <mutex>

int main() {
  std::mutex outMutex;
  drpc::Server server;

  server.onClientConnected([&](const drpc::ClientInfo &c) {
    std::lock_guard<std::mutex> lk(outMutex);
    std::cout << "[+] app " << c.client_id << " connected\n";
  });
  server.onClientDisconnected([&](const drpc::ClientInfo &c) {
    std::lock_guard<std::mutex> lk(outMutex);
    std::cout << "[-] app " << c.client_id << " disconnected\n";
  });
  server.onActivity([&](const drpc::ClientInfo &c, const std::optional<drpc::Activity> &a) {
    std::lock_guard<std::mutex> lk(outMutex);
    if (!a) {
      std::cout << "[" << c.client_id << "] presence cleared\n";
      return;
    }
    std::cout << "[" << c.client_id << " pid=" << c.pid << "] " << a->toJson().dump(2) << "\n";
  });

  if (!server.start()) {
    std::cerr << "could not bind any discord-ipc-N endpoint\n";
    return 1;
  }
  std::cout << "listening on " << server.endpoint() << "\nPress Enter to quit.\n";
  std::cin.get();
  server.stop();
}
