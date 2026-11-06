#include "protocol.hpp"

namespace drpc {

namespace {
constexpr uint32_t kMaxPayload = 1u << 20;

void putU32(std::string &out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t getU32(const unsigned char *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
} // namespace

bool writeFrame(Connection &c, Opcode op, const std::string &payload) {
  std::string buf;
  buf.reserve(8 + payload.size());
  putU32(buf, static_cast<uint32_t>(op));
  putU32(buf, static_cast<uint32_t>(payload.size()));
  buf += payload;
  return c.write(buf.data(), buf.size());
}

bool readFrame(Connection &c, uint32_t &op, std::string &payload) {
  unsigned char hdr[8];
  if (!c.read(hdr, sizeof(hdr))) return false;
  op = getU32(hdr);
  uint32_t len = getU32(hdr + 4);
  if (len > kMaxPayload) return false;
  payload.assign(len, '\0');
  return len == 0 || c.read(&payload[0], len);
}

std::string dumpJson(const nlohmann::json &j) { return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); }

nlohmann::json parseJson(const std::string &s) { return nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false); }

} // namespace drpc
