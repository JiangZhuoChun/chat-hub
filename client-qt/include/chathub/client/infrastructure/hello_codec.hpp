#pragma once

#include "chathub/contracts/hello.hpp"
#include "chathub/contracts/ids.hpp"
#include "chathub/contracts/outcome.hpp"

// 客户端 hello 帧 JSON codec（D66、D198）：Qt JSON ↔ 纯值对象映射。
// 只做 wire 层校验；协商/版本判断归上层。

#include <string>
#include <string_view>

namespace chathub::client::infrastructure {

// 解析 D68 信封的 hello_response（成功 ok=true）。
contracts::Outcome<contracts::HelloResponse> decodeHelloResponse(
    std::string_view json, contracts::RequestId& out_request_id);


// 编码 D68 信封的 hello_request（含 UUID request_id）为紧凑 UTF-8 JSON。
contracts::Outcome<std::string> encodeHelloRequest(const contracts::RequestId& request_id,
                               const contracts::HelloRequest& request);

}  // namespace chathub::client::infrastructure