#pragma once
#include "connection.hpp"
#include "drpc/drpc.hpp"

namespace drpc {

bool writeFrame(Connection &c, Opcode op, const std::string &payload);
bool readFrame(Connection &c, uint32_t &op, std::string &payload);

std::string dumpJson(const nlohmann::json &j);
nlohmann::json parseJson(const std::string &s);

} // namespace drpc
