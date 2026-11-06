#include "connection.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace drpc {

struct Connection::Impl {
  int fd = -1;
};

Connection::Connection() : impl_(new Impl) {}
Connection::~Connection() { close(); }

namespace {

std::vector<std::string> envBaseDirs() {
  std::vector<std::string> bases;
  for (const char *var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
    const char *v = std::getenv(var);
    if (v && *v) bases.emplace_back(v);
  }
  bases.emplace_back("/tmp");
  for (auto &b : bases)
    while (b.size() > 1 && b.back() == '/') b.pop_back();
  return bases;
}

std::vector<std::string> clientCandidateDirs() {
  static const char *subdirs[] = {
    "",
    "/app/com.discordapp.Discord",
    "/app/com.discordapp.DiscordCanary",
    "/snap.discord",
    "/snap.discord-canary",
  };
  std::vector<std::string> out;
  for (const auto &b : envBaseDirs())
    for (const char *s : subdirs) {
      std::string d = b + s;
      if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    }
  return out;
}

bool makeAddr(const std::string &path, sockaddr_un &addr) {
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) return false;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  return true;
}

void configureSocket(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  timeval tv{5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

} // namespace

bool Connection::open() {
  close();
  const auto dirs = clientCandidateDirs();
  for (int i = 0; i < 10; ++i) {
    for (const auto &dir : dirs) {
      sockaddr_un addr;
      if (!makeAddr(dir + "/discord-ipc-" + std::to_string(i), addr)) continue;
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) continue;
      if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        continue;
      }
      configureSocket(fd);
      impl_->fd = fd;
      return true;
    }
  }
  return false;
}

void Connection::close() {
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
}

bool Connection::isOpen() const { return impl_->fd >= 0; }

bool Connection::write(const void *data, size_t len) {
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  const char *p = static_cast<const char *>(data);
  while (len > 0) {
    ssize_t n = ::send(impl_->fd, p, len, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool Connection::read(void *data, size_t len) {
  char *p = static_cast<char *>(data);
  while (len > 0) {
    ssize_t n = ::recv(impl_->fd, p, len, 0);
    if (n == 0) return false; // peer closed
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

int Connection::waitReadable(int timeout_ms) {
  pollfd pfd{impl_->fd, POLLIN, 0};
  int r = ::poll(&pfd, 1, timeout_ms);
  if (r < 0) return errno == EINTR ? 0 : -1;
  if (r == 0) return 0;
  if (pfd.revents & POLLIN) return 1;
  return -1;
}

struct Listener::Impl {
  int fd = -1;
  std::string path;
};

Listener::Listener() : impl_(new Impl) {}
Listener::~Listener() { close(); }

bool Listener::listen(const std::string &dir_override) {
  close();
  std::string dir = dir_override;
  if (dir.empty()) dir = envBaseDirs().front();
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();

  for (int i = 0; i < 10; ++i) {
    const std::string path = dir + "/discord-ipc-" + std::to_string(i);
    sockaddr_un addr;
    if (!makeAddr(path, addr)) return false;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      if (errno == EADDRINUSE) {
        int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        bool live = probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
        if (probe >= 0) ::close(probe);
        if (!live) {
          ::unlink(path.c_str());
          if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) goto bound;
        }
      }
      ::close(fd);
      continue;
    }
  bound:
    if (::listen(fd, 16) != 0) {
      ::close(fd);
      ::unlink(path.c_str());
      continue;
    }
    impl_->fd = fd;
    impl_->path = path;
    return true;
  }
  return false;
}

void Listener::close() {
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    ::unlink(impl_->path.c_str());
    impl_->fd = -1;
    impl_->path.clear();
  }
}

std::unique_ptr<Connection> Listener::accept(int timeout_ms) {
  if (impl_->fd < 0) return nullptr;
  pollfd pfd{impl_->fd, POLLIN, 0};
  int r = ::poll(&pfd, 1, timeout_ms);
  if (r <= 0) {
    if (r < 0 && errno != EINTR) usleep(100 * 1000);
    return nullptr;
  }
  int fd = ::accept(impl_->fd, nullptr, nullptr);
  if (fd < 0) return nullptr;
  configureSocket(fd);
  std::unique_ptr<Connection> c(new Connection());
  c->impl_->fd = fd;
  return c;
}

const std::string &Listener::endpoint() const { return impl_->path; }

} // namespace drpc
