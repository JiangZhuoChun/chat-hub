// M1-7 Task 1：hello 值类型与 capability 交集（D09、D10、D55、D72、D73、D197）。

#include <cstdio>

#include "chathub/contracts/hello.hpp"

#define CHECK(expr)                                      \
do {                                                   \
if (!(expr)) {                                       \
std::fprintf(stderr, "CHECK failed: %s\n", #expr); \
return 1;                                          \
}                                                    \
} while (0)

int main() {
  using namespace chathub::contracts;

  // 合同常量锚定：防止服务端连接参数漂移（D09、D10、D73）。
  CHECK(kMaxJsonBytes == 65536);
  CHECK(kMaxTextBytes == 4096);
  CHECK(kHeartbeatIdleMs == 20000);
  CHECK(kSessionTimeoutMs == 60000);

  // HelloResponse 默认构造即填充服务端下发参数（D72 逐字段）。
  HelloResponse response;
  CHECK(response.server_version == kServerVersion);
  CHECK(response.max_json == kMaxJsonBytes);
  CHECK(response.max_text == kMaxTextBytes);
  CHECK(response.heartbeat_idle == kHeartbeatIdleMs);
  CHECK(response.timeout == kSessionTimeoutMs);

  // 交集：完全重叠取到 text_v1。
  const CapabilitySet both{{kCapabilityTextV1, "file_v1"}};
  const CapabilitySet server{{kCapabilityTextV1}};
  const auto negotiated = CapabilitySet::intersect(server, both);
  CHECK(negotiated.size() == 1);
  CHECK(negotiated.contains(kCapabilityTextV1));
  CHECK(!negotiated.contains("file_v1"));  // 服务端不支持的未来能力被裁掉

  // 交集：无重叠为空。
  const CapabilitySet empty = CapabilitySet::intersect(server, CapabilitySet{});
  CHECK(empty.empty());

  // 去重：同名 capability 只保留一个。
  const CapabilitySet dup{{kCapabilityTextV1, kCapabilityTextV1}};
  CHECK(dup.size() == 1);

  return 0;
}