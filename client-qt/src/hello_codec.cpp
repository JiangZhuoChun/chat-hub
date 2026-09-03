#include "chathub/client/infrastructure/hello_codec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <cmath>
#include <cstdint>
#include <utility>

namespace chathub::client::infrastructure {

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

constexpr double kMaxSafeInteger = 9007199254740991.0;  // 2^53-1（D67）

}  // namespace

std::string encodeHelloRequest(const RequestId& request_id,
                               const HelloRequest& request) {
  QJsonObject data;
  data.insert(QStringLiteral("client_version"),
              QString::fromStdString(request.client_version));
  data.insert(QStringLiteral("platform"),
              QString::fromStdString(request.platform));

  QJsonArray caps;
  for (const auto& c : request.capabilities.value()) {
    caps.append(QString::fromStdString(c));
  }
  data.insert(QStringLiteral("capabilities"), caps);

  QJsonObject root;
  root.insert(QStringLiteral("request_id"),
              QString::fromStdString(request_id.value()));
  root.insert(QStringLiteral("data"), data);

  return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

Outcome<HelloResponse> decodeHelloResponse(std::string_view json,
                                           RequestId& out_request_id) {
  const QByteArray bytes(json.data(), static_cast<qint64>(json.size()));
  QJsonParseError parse_error{};
  const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || doc.isNull()) {
    return invalidJson("hello_response 非合法 JSON");
  }
  if (!doc.isObject()) {
    return invalidRequest("hello_response 根必须是对象");
  }
  const QJsonObject root = doc.object();

  // request_id（D69）
  const QJsonValue id_v = root.value(QStringLiteral("request_id"));
  if (!id_v.isString()) {
    return invalidRequestId("request_id 缺失或非字符串");
  }
  auto parsed_id =
      RequestId::parse(id_v.toString().toStdString());
  if (!parsed_id) {
    return invalidRequestId("request_id 非合法 UUID");
  }
  out_request_id = std::move(*parsed_id);

  // hello 无业务失败：只接受成功语义 ok=true。
  const QJsonValue ok_v = root.value(QStringLiteral("ok"));
  if (!ok_v.isBool() || !ok_v.toBool()) {
    return invalidRequest("hello_response ok 非 true");
  }

  const QJsonValue data_v = root.value(QStringLiteral("data"));
  if (!data_v.isObject()) {
    return invalidRequest("data 缺失或非对象");
  }
  const QJsonObject data = data_v.toObject();

  HelloResponse out;

  // server_version：字符串、非空、≤ 32 字节。
  const QJsonValue ver_v = data.value(QStringLiteral("server_version"));
  if (!ver_v.isString()) {
    return invalidRequest("server_version 缺失或非字符串");
  }
  const std::string ver =
      ver_v.toString().toStdString();
  if (ver.empty() || ver.size() > contracts::kMaxClientVersionBytes) {
    return invalidRequest("server_version 长度非法");
  }
  out.server_version = ver;

  // server_time：非负整数 ≤ 2^53-1（秒级 Unix 时间戳）。
  const QJsonValue time_v = data.value(QStringLiteral("server_time"));
  if (!time_v.isDouble()) {
    return invalidRequest("server_time 非数值");
  }
  const double time_d = time_v.toDouble();
  if (time_d < 0.0 || time_d > kMaxSafeInteger ||
      std::floor(time_d) != time_d) {
    return invalidRequest("server_time 非法");
  }
  out.server_time = static_cast<std::int64_t>(time_d);

  // max_json/max_text/heartbeat_idle/timeout：非负整数 ≤ 2^32-1。
  const auto read_uint32 = [&](const char* key, std::uint32_t& target) {
    const QJsonValue v = data.value(QString::fromLatin1(key));
    if (!v.isDouble()) {
      return false;
    }
    const double d = v.toDouble();
    if (d < 0.0 || d > 4294967295.0 || std::floor(d) != d) {
      return false;
    }
    target = static_cast<std::uint32_t>(d);
    return true;
  };
  if (!read_uint32("max_json", out.max_json) ||
      !read_uint32("max_text", out.max_text) ||
      !read_uint32("heartbeat_idle", out.heartbeat_idle) ||
      !read_uint32("timeout", out.timeout)) {
    return invalidRequest("连接参数字段缺失或非法");
  }

  // capabilities：数组、≤16 项、每项非空字符串 ≤64 字节。
  const QJsonValue cap_v = data.value(QStringLiteral("capabilities"));
  if (!cap_v.isArray()) {
    return invalidRequest("capabilities 缺失或非数组");
  }
  const QJsonArray cap_arr = cap_v.toArray();
  if (cap_arr.size() > static_cast<qint64>(contracts::kMaxCapabilityCount)) {
    return invalidRequest("capabilities 数量超限");
  }
  for (const auto item : cap_arr) {
    if (!item.isString()) {
      return invalidRequest("capability 非字符串");
    }
    const std::string c =
        item.toString().toStdString();
    if (c.empty() || c.size() > contracts::kMaxCapabilityValueBytes) {
      return invalidRequest("capability 长度非法");
    }
    out.capabilities.insert(c);
  }

  return out;
}

}  // namespace chathub::client::infrastructure