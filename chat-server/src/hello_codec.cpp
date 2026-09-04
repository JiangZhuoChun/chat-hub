#include "chathub/server/transport/hello_codec.hpp"

#include <boost/json.hpp>
#include <utility>

namespace chathub::server::transport {

using contracts::Error;
using contracts::HelloRequest;
using contracts::HelloResponse;
using contracts::Outcome;
using contracts::RequestId;

namespace {

constexpr std::string_view kInvalidJson = "invalid_json";
constexpr std::string_view kInvalidRequestId = "invalid_request_id";
constexpr std::string_view kInvalidRequest = "invalid_request";

[[nodiscard]] Error invalidJson(std::string detail) {
  return Error{kInvalidJson, std::move(detail)};
}
[[nodiscard]] Error invalidRequestId(std::string detail) {
  return Error{kInvalidRequestId, std::move(detail)};
}
[[nodiscard]] Error invalidRequest(std::string detail) {
  return Error{kInvalidRequest, std::move(detail)};
}

}  // namespace

Outcome<HelloRequest> decodeHelloRequest(std::string_view json,
                                         RequestId& out_request_id) {

  if (json.size() > contracts::kMaxJsonBytes) {
    return invalidRequest("正文超过 64 KiB");
  }

  boost::system::error_code parse_ec;
  const boost::json::value root = boost::json::parse(json, parse_ec);
  if (parse_ec) {
    return invalidJson("hello_request 非合法 JSON");
  }
  if (!root.is_object()) {
    return invalidRequest("hello_request 根必须是对象");
  }
  const auto& obj = root.as_object();

  // request_id（D69）：必须存在、字符串、合法 UUID v4；存储统一小写。
  const auto id_it = obj.find("request_id");
  if (id_it == obj.end() || !id_it->value().is_string()) {
    return invalidRequestId("request_id 缺失或非字符串");
  }
  const auto id_sv = id_it->value().as_string();
  auto parsed_id =
      RequestId::parse(std::string_view(id_sv.data(), id_sv.size()));
  if (!parsed_id) {
    return invalidRequestId("request_id 非合法 UUID");
  }
  out_request_id = std::move(*parsed_id);

  // data（D68）：必须存在且为对象。
  const auto data_it = obj.find("data");
  if (data_it == obj.end() || !data_it->value().is_object()) {
    return invalidRequest("data 缺失或非对象");
  }
  const auto& data = data_it->value().as_object();

  HelloRequest out;

  // client_version：字符串、非空、≤ 32 字节（取值是否受支持由 M1-8 handler
  // 判断）。
  const auto cv_it = data.find("client_version");
  if (cv_it == data.end() || !cv_it->value().is_string()) {
    return invalidRequest("client_version 缺失或非字符串");
  }
  const auto cv_sv = cv_it->value().as_string();
  if (cv_sv.empty() || cv_sv.size() > contracts::kMaxClientVersionBytes) {
    return invalidRequest("client_version 长度非法");
  }
  out.client_version.assign(cv_sv.data(), cv_sv.size());

  // platform：字符串、非空、≤ 16 字节。
  const auto pf_it = data.find("platform");
  if (pf_it == data.end() || !pf_it->value().is_string()) {
    return invalidRequest("platform 缺失或非字符串");
  }
  const auto pf_sv = pf_it->value().as_string();
  if (pf_sv.empty() || pf_sv.size() > contracts::kMaxPlatformBytes) {
    return invalidRequest("platform 长度非法");
  }
  out.platform.assign(pf_sv.data(), pf_sv.size());

  // capabilities：数组、≤16 项、每项非空字符串且 ≤64
  // 字节；未知值由交集裁掉（D197）。
  const auto cap_it = data.find("capabilities");
  if (cap_it == data.end() || !cap_it->value().is_array()) {
    return invalidRequest("capabilities 缺失或非数组");
  }
  const auto& cap_arr = cap_it->value().as_array();
  if (cap_arr.size() > contracts::kMaxCapabilityCount) {
    return invalidRequest("capabilities 数量超限");
  }
  for (const auto& item : cap_arr) {
    if (!item.is_string()) {
      return invalidRequest("capability 非字符串");
    }
    const auto cap_sv = item.as_string();
    if (cap_sv.empty() || cap_sv.size() > contracts::kMaxCapabilityValueBytes) {
      return invalidRequest("capability 长度非法");
    }
    out.capabilities.insert(std::string_view(cap_sv.data(), cap_sv.size()));
  }

  return out;
}

std::string encodeHelloResponse(const RequestId& request_id,
                                const HelloResponse& response) {
  boost::json::object data;
  data["server_version"] = response.server_version;
  data["server_time"] = response.server_time;  // 秒级 Unix 时间戳（本步确认）
  data["max_json"] = response.max_json;
  data["max_text"] = response.max_text;
  data["heartbeat_idle"] = response.heartbeat_idle;
  data["timeout"] = response.timeout;

  boost::json::array caps;
  for (const auto& c : response.capabilities.value()) {
    caps.emplace_back(c);
  }
  data["capabilities"] = std::move(caps);

  boost::json::object root;
  root["request_id"] = request_id.value();  // 回显规范化小写 UUID（D13/D68）
  root["ok"] = true;
  root["data"] = std::move(data);

  return boost::json::serialize(root);  // 紧凑 UTF-8 JSON（D66）
}

}  // namespace chathub::server::transport