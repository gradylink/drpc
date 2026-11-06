#include "drpc/drpc.hpp"

#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <application_id>\n";
    return 1;
  }

  drpc::Client client(argv[1]);
  client.onReady([](const nlohmann::json &data) { std::cout << "connected as " << data.value("user", nlohmann::json::object()).value("username", "?") << "\n"; });
  client.onError([](int code, const std::string &msg) { std::cerr << "discord error " << code << ": " << msg << "\n"; });
  client.onDisconnected([](int code, const std::string &msg) { std::cerr << "disconnected (" << code << "): " << msg << " - will retry\n"; });
  client.start();

  const auto now = std::chrono::system_clock::now().time_since_epoch();
  drpc::Activity a;
  a.details = "custom rpc implementation";
  a.state = "in C++ :)";
  a.start_timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  // a.large_image = "your_asset_key";
  // a.buttons.push_back({"Source", "https://example.com"});
  client.setActivity(a);

  std::cout << "Presence set. Press Enter to quit.\n";
  std::cin.get();
  client.clearActivity();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  client.stop();
}
