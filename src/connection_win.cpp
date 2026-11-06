#include "connection.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace drpc {

struct Connection::Impl {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool overlapped = false;
  bool server = false;
};

Connection::Connection() : impl_(new Impl) {}
Connection::~Connection() { close(); }

static std::wstring pipeName(int i) { return L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(i); }

bool Connection::open() {
  close();
  for (int i = 0; i < 10; ++i) {
    std::wstring path = pipeName(i);
    for (int attempt = 0; attempt < 2; ++attempt) {
      HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        impl_->h = h;
        return true;
      }
      if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(path.c_str(), 200)) break;
    }
  }
  return false;
}

void Connection::close() {
  if (impl_->h != INVALID_HANDLE_VALUE) {
    if (impl_->server) {
      FlushFileBuffers(impl_->h);
      DisconnectNamedPipe(impl_->h);
    }
    CloseHandle(impl_->h);
    impl_->h = INVALID_HANDLE_VALUE;
  }
}

bool Connection::isOpen() const { return impl_->h != INVALID_HANDLE_VALUE; }

static bool ioOnce(HANDLE h, bool overlapped, bool isWrite, void *buf, DWORD len, DWORD &n) {
  n = 0;
  if (!overlapped) {
    return isWrite ? WriteFile(h, buf, len, &n, nullptr) : ReadFile(h, buf, len, &n, nullptr);
  }
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return false;
  BOOL ok = isWrite ? WriteFile(h, buf, len, nullptr, &ov) : ReadFile(h, buf, len, nullptr, &ov);
  if (!ok && GetLastError() != ERROR_IO_PENDING) {
    CloseHandle(ov.hEvent);
    return false;
  }
  ok = GetOverlappedResult(h, &ov, &n, TRUE);
  CloseHandle(ov.hEvent);
  return ok;
}

bool Connection::write(const void *data, size_t len) {
  const char *p = static_cast<const char *>(data);
  while (len > 0) {
    DWORD n = 0;
    if (!ioOnce(impl_->h, impl_->overlapped, true, const_cast<char *>(p), static_cast<DWORD>(len), n) || n == 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

bool Connection::read(void *data, size_t len) {
  char *p = static_cast<char *>(data);
  while (len > 0) {
    DWORD n = 0;
    if (!ioOnce(impl_->h, impl_->overlapped, false, p, static_cast<DWORD>(len), n) || n == 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

int Connection::waitReadable(int timeout_ms) {
  int waited = 0;
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(impl_->h, nullptr, 0, nullptr, &avail, nullptr)) return -1;
    if (avail > 0) return 1;
    if (waited >= timeout_ms) return 0;
    Sleep(10);
    waited += 10;
  }
}

struct Listener::Impl {
  std::wstring name;
  std::string endpoint;
  HANDLE inst = INVALID_HANDLE_VALUE;
  HANDLE evt = nullptr;
  OVERLAPPED ov{};
  bool pending = false;
};

Listener::Listener() : impl_(new Impl) {}
Listener::~Listener() { close(); }

static HANDLE createInstance(const std::wstring &name, bool first) {
  DWORD mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first) mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  return CreateNamedPipeW(name.c_str(), mode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
}

bool Listener::listen(const std::string &) {
  close();
  for (int i = 0; i < 10; ++i) {
    std::wstring name = pipeName(i);
    HANDLE h = createInstance(name, true);
    if (h == INVALID_HANDLE_VALUE) continue;
    impl_->name = name;
    impl_->endpoint = "\\\\?\\pipe\\discord-ipc-" + std::to_string(i);
    impl_->inst = h;
    impl_->evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return impl_->evt != nullptr;
  }
  return false;
}

void Listener::close() {
  if (impl_->inst != INVALID_HANDLE_VALUE) {
    if (impl_->pending) CancelIoEx(impl_->inst, &impl_->ov);
    CloseHandle(impl_->inst);
    impl_->inst = INVALID_HANDLE_VALUE;
  }
  if (impl_->evt) {
    CloseHandle(impl_->evt);
    impl_->evt = nullptr;
  }
  impl_->pending = false;
  impl_->endpoint.clear();
}

std::unique_ptr<Connection> Listener::accept(int timeout_ms) {
  if (!impl_->evt) return nullptr;
  if (impl_->inst == INVALID_HANDLE_VALUE) {
    impl_->inst = createInstance(impl_->name, false);
    if (impl_->inst == INVALID_HANDLE_VALUE) {
      Sleep(100);
      return nullptr;
    }
  }
  bool connected = false;
  if (!impl_->pending) {
    impl_->ov = OVERLAPPED{};
    impl_->ov.hEvent = impl_->evt;
    ResetEvent(impl_->evt);
    if (ConnectNamedPipe(impl_->inst, &impl_->ov)) {
      connected = true;
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) connected = true;
      else if (err == ERROR_IO_PENDING) impl_->pending = true;
      else {
        CloseHandle(impl_->inst);
        impl_->inst = INVALID_HANDLE_VALUE;
        return nullptr;
      }
    }
  }
  if (!connected) {
    if (WaitForSingleObject(impl_->evt, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) return nullptr;
    DWORD n = 0;
    impl_->pending = false;
    if (!GetOverlappedResult(impl_->inst, &impl_->ov, &n, FALSE)) {
      CloseHandle(impl_->inst);
      impl_->inst = INVALID_HANDLE_VALUE;
      return nullptr;
    }
  }
  std::unique_ptr<Connection> c(new Connection());
  c->impl_->h = impl_->inst;
  c->impl_->overlapped = true;
  c->impl_->server = true;
  impl_->inst = INVALID_HANDLE_VALUE;
  return c;
}

const std::string &Listener::endpoint() const { return impl_->endpoint; }

} // namespace drpc
