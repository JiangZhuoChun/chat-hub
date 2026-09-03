#pragma once

// 服务端 hello 帧 JSON codec（D66、D198、D200）：Boost.JSON ↔ 纯值对象映射。
// 只做 wire 层校验（字段存在/类型/长度/request_id 格式），业务版本判断归
// M1-8 的 hello handler（codec 不判 client_version/platform 的取值）。

#include <string>
#include <string_view>

#include "chathub/contracts/hello.hpp"
#include "chathub/contracts/ids.hpp"
#include "chathub/contracts/outcome.hpp"

namespace chathub::server::transport {

// 解析 D68 信封的 hello_request 正文（不含帧头）。
contracts::Outcome<contracts::HelloRequest> decodeHelloRequest(
    std::string_view json, contracts::RequestId& out_request_id);

// 编码 D68 信封的 hello_response（成功 ok=true），输出紧凑 UTF-8 JSON 字符串。
std::string encodeHelloResponse(const contracts::RequestId& request_id,
                                const contracts::HelloResponse& response);

}  // namespace chathub::server::transport