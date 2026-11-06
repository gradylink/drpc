#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace drpc {

class Connection {
public:
  Connection();
  ~Connection();
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

  bool open();
  void close();
  bool isOpen() const;

  bool write(const void *data, size_t len);
  bool read(void *data, size_t len);
  int waitReadable(int timeout_ms);

private:
  friend class Listener;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Listener {
public:
  Listener();
  ~Listener();
  Listener(const Listener &) = delete;
  Listener &operator=(const Listener &) = delete;

  bool listen(const std::string &dir_override);
  void close();
  std::unique_ptr<Connection> accept(int timeout_ms);
  const std::string &endpoint() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace drpc
