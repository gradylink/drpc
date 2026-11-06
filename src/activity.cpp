#include "drpc/drpc.hpp"

namespace drpc {

using json = nlohmann::json;

namespace {
std::string getStr(const json &j, const char *key) {
  if (!j.is_object()) return {};
  auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
std::optional<int64_t> getInt(const json &j, const char *key) {
  if (!j.is_object()) return std::nullopt;
  auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return std::nullopt;
  return it->get<int64_t>();
}
json subObject(const json &j, const char *key) {
  if (!j.is_object()) return json::object();
  auto it = j.find(key);
  return (it != j.end() && it->is_object()) ? *it : json::object();
}
} // namespace

json Activity::toJson() const {
  json a = json::object();
  if (!details.empty()) a["details"] = details;
  if (!state.empty()) a["state"] = state;

  json ts = json::object();
  if (start_timestamp) ts["start"] = *start_timestamp;
  if (end_timestamp) ts["end"] = *end_timestamp;
  if (!ts.empty()) a["timestamps"] = ts;

  json assets = json::object();
  if (!large_image.empty()) assets["large_image"] = large_image;
  if (!large_text.empty()) assets["large_text"] = large_text;
  if (!small_image.empty()) assets["small_image"] = small_image;
  if (!small_text.empty()) assets["small_text"] = small_text;
  if (!assets.empty()) a["assets"] = assets;

  json party = json::object();
  if (!party_id.empty()) party["id"] = party_id;
  if (party_max > 0) party["size"] = json::array({party_size, party_max});
  if (!party.empty()) a["party"] = party;

  json secrets = json::object();
  if (!match_secret.empty()) secrets["match"] = match_secret;
  if (!join_secret.empty()) secrets["join"] = join_secret;
  if (!spectate_secret.empty()) secrets["spectate"] = spectate_secret;
  if (!secrets.empty()) a["secrets"] = secrets;

  if (!buttons.empty()) {
    json arr = json::array();
    for (const auto &b : buttons) arr.push_back({{"label", b.label}, {"url", b.url}});
    a["buttons"] = arr;
  }
  if (instance) a["instance"] = true;
  return a;
}

Activity Activity::fromJson(const json &j) {
  Activity a;
  if (!j.is_object()) return a;
  a.details = getStr(j, "details");
  a.state = getStr(j, "state");

  json ts = subObject(j, "timestamps");
  a.start_timestamp = getInt(ts, "start");
  a.end_timestamp = getInt(ts, "end");

  json assets = subObject(j, "assets");
  a.large_image = getStr(assets, "large_image");
  a.large_text = getStr(assets, "large_text");
  a.small_image = getStr(assets, "small_image");
  a.small_text = getStr(assets, "small_text");

  json party = subObject(j, "party");
  a.party_id = getStr(party, "id");
  if (party.contains("size") && party["size"].is_array() && party["size"].size() == 2 && party["size"][0].is_number() && party["size"][1].is_number()) {
    a.party_size = party["size"][0].get<int>();
    a.party_max = party["size"][1].get<int>();
  }

  json secrets = subObject(j, "secrets");
  a.match_secret = getStr(secrets, "match");
  a.join_secret = getStr(secrets, "join");
  a.spectate_secret = getStr(secrets, "spectate");

  auto bit = j.find("buttons");
  if (bit != j.end() && bit->is_array()) {
    for (const auto &b : *bit) a.buttons.push_back({getStr(b, "label"), getStr(b, "url")});
  }
  auto iit = j.find("instance");
  a.instance = iit != j.end() && iit->is_boolean() && iit->get<bool>();
  return a;
}

std::string Activity::validate() const {
  if (details.size() > 128) return "details must be at most 128 characters";
  if (state.size() > 128) return "state must be at most 128 characters";
  if (buttons.size() > 2) return "at most 2 buttons are allowed";
  for (const auto &b : buttons) {
    if (b.label.empty() || b.label.size() > 32) return "button label must be 1-32 characters";
    if (b.url.rfind("http://", 0) != 0 && b.url.rfind("https://", 0) != 0) return "button url must start with http:// or https://";
  }
  if (party_max > 0 && (party_size < 0 || party_size > party_max)) return "party size must be between 0 and party max";
  if (start_timestamp && end_timestamp && *end_timestamp < *start_timestamp) return "end timestamp is before start timestamp";
  return {};
}

} // namespace drpc
