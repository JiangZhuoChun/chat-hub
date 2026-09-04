// M1-7 Task 2：hello 双端 JSON codec（D66、D68、D197、D198）。
// 验证：服务端 Boost.JSON 与客户端 Qt JSON 各自映射 D68 信封，对同一 fixture
// 往返一致；wire 层负例（语法/字段/类型/长度/request_id）被拒绝。

#include "chathub/client/infrastructure/hello_codec.hpp"

#include <cstdio>
#include <string>

#include "chathub/contracts/hello.hpp"
#include "chathub/contracts/ids.hpp"
#include "chathub/contracts/protocol_descriptor.hpp"
#include "chathub/server/transport/hello_codec.hpp"

#define CHECK(expr)                                      \
  do {                                                   \
    if (!(expr)) {                                       \
      std::fprintf(stderr, "CHECK failed: %s\n", #expr); \
      return 1;                                          \
    }                                                    \
  } while (0)

int main() {
  using namespace chathub::contracts;
  using chathub::client::infrastructure::decodeHelloResponse;
  using chathub::client::infrastructure::encodeHelloRequest;
  using chathub::server::transport::decodeHelloRequest;
  using chathub::server::transport::encodeHelloResponse;

  const std::string request_json =
      R"({"request_id":"6ba7b810-9dad-41d1-80b4-00c04fd430c8","data":{"client_version":"1.0","platform":"windows","capabilities":["text_v1","file_v1","text_v1"]}})";

  // 1. 服务端 decode 合法 request（含 capability 去重，未知值保留）。
  RequestId req_id;
  auto r = decodeHelloRequest(request_json, req_id);
  CHECK(isOk(r));
  const auto& req = value(r);
  CHECK(req.client_version == "1.0");
  CHECK(req.platform == "windows");
  CHECK(req.capabilities.size() == 2);          // text_v1 重复只留一个
  CHECK(req.capabilities.contains("text_v1"));
  CHECK(req.capabilities.contains("file_v1"));  // 未知值保留，交交集处理
  CHECK(req_id.value() == "6ba7b810-9dad-41d1-80b4-00c04fd430c8");

  // 2. 客户端 encode 同一 request → 服务端 decode，往返等价。
  const auto client_encoded = encodeHelloRequest(req_id, req);
  CHECK(isOk(client_encoded));
  const auto& client_encoded_ok = value(client_encoded);
  RequestId req_id2;
  auto r2 = decodeHelloRequest(client_encoded_ok, req_id2);
  CHECK(isOk(r2));
  CHECK(value(r2).client_version == "1.0");
  CHECK(value(r2).platform == "windows");
  CHECK(value(r2).capabilities.size() == req.capabilities.size());
  CHECK(req_id2.value() == req_id.value());

  // 3. 服务端 encode response → 客户端 decode，往返等价。
  HelloResponse resp;
  resp.server_time = 1700000000;                       // 秒级 Unix 时间戳
  resp.capabilities = CapabilitySet{{kCapabilityTextV1}};
  const std::string resp_json = encodeHelloResponse(req_id, resp);
  RequestId resp_id;
  auto r3 = decodeHelloResponse(resp_json, resp_id);
  CHECK(isOk(r3));
  const auto& decoded = value(r3);
  CHECK(decoded.server_version == kServerVersion);
  CHECK(decoded.server_time == 1700000000);
  CHECK(decoded.max_json == 65536);
  CHECK(decoded.max_text == 4096);
  CHECK(decoded.heartbeat_idle == 20000);
  CHECK(decoded.timeout == 60000);
  CHECK(decoded.capabilities.size() == 1);
  CHECK(decoded.capabilities.contains(kCapabilityTextV1));
  CHECK(resp_id.value() == req_id.value());

  // 4. 服务端 decode 负例。
  RequestId scratch;
  auto bad_json = decodeHelloRequest("{", scratch);
  CHECK(!isOk(bad_json) && error(bad_json).code == "invalid_json");

  auto bad_id =
      decodeHelloRequest(R"({"request_id":"nope","data":{}})", scratch);
  CHECK(!isOk(bad_id) && error(bad_id).code == "invalid_request_id");

  auto no_data = decodeHelloRequest(
      R"({"request_id":"6ba7b810-9dad-41d1-80b4-00c04fd430c8"})", scratch);
  CHECK(!isOk(no_data) && error(no_data).code == "invalid_request");

  auto no_cv = decodeHelloRequest(
      R"({"request_id":"6ba7b810-9dad-41d1-80b4-00c04fd430c8","data":{"platform":"windows","capabilities":[]}})",
      scratch);
  CHECK(!isOk(no_cv) && error(no_cv).code == "invalid_request");

  // capability 数量超限（17 项）。
  std::string many_caps =
      R"({"request_id":"6ba7b810-9dad-41d1-80b4-00c04fd430c8","data":{"client_version":"1.0","platform":"windows","capabilities":[)";
  for (int i = 0; i < 17; ++i) {
    if (i) many_caps += ",";
    many_caps += "\"text_v1\"";
  }
  many_caps += "]}}";
  auto cap_too_many = decodeHelloRequest(many_caps, scratch);
  CHECK(!isOk(cap_too_many) && error(cap_too_many).code == "invalid_request");

  // 5. 客户端 decode 负例。
  auto c_bad_json = decodeHelloResponse("{", scratch);
  CHECK(!isOk(c_bad_json) && error(c_bad_json).code == "invalid_json");

  auto c_ok_false = decodeHelloResponse(
      R"({"request_id":"6ba7b810-9dad-41d1-80b4-00c04fd430c8","ok":false,"error":{"code":"x","message":"y"}})",
      scratch);
  CHECK(!isOk(c_ok_false) && error(c_ok_false).code == "invalid_request");

  auto c_no_max = decodeHelloResponse(
      R"({"request_id":"6ba7b810-9dad-41d1-80b4-00c04fd430c8","ok":true,"data":{"server_version":"1.0","server_time":0}})",
      scratch);
  CHECK(!isOk(c_no_max) && error(c_no_max).code == "invalid_request");

  // 正文超过 64 KiB 必须在分配/解析前拒绝。
  std::string oversized(kMaxJsonBody + 1, ' ');
  auto big = decodeHelloRequest(oversized, scratch);
  CHECK(!isOk(big) && error(big).code == "invalid_request");
  auto c_big = decodeHelloResponse(oversized, scratch);
  CHECK(!isOk(c_big) && error(c_big).code == "invalid_request");

  return 0;
}