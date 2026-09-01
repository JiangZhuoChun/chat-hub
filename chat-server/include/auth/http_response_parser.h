#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// ==================== 模块：HTTP introspection 响应解析 ====================
// 功能：集中解析 Auth Service introspection 接口的 HTTP 响应，全部为纯函数，
//       不持有状态、不做网络 I/O，便于独立单元测试。
namespace auth {

// 功能：从 HTTP 状态行（如 "HTTP/1.1 200 OK"）中解析出三位状态码。
// 失败：非 "HTTP/" 前缀、缺少空格分隔、状态码非纯数字或超出 100..599 时返回 std::nullopt。
std::optional<int> parseHttpStatusCode(std::string_view status_line);

// 功能：在响应头部区扫描 Content-Length 字段并解析 body 字节数；
//       允许调用方把状态行一起传入，且只接受恰好出现一次的 Content-Length。
// 失败：字段缺失、重复、值空、非十进制整数、超过 max_body_bytes 或头部格式损坏时返回 std::nullopt。
std::optional<std::size_t> parseContentLength(
    std::string_view response_headers, std::size_t max_body_bytes) noexcept;

// 功能：解析成功响应（200）的 JSON 正文，返回 active=true 时的认证用户名。
// 失败：JSON 损坏、非对象、缺少 active/username、active 不为 true、
//       username 非字符串或为空时返回 std::nullopt。
std::optional<std::string> parseActiveUsername(std::string_view response_body);

// 功能：解析错误响应（401）的 JSON 正文中的 code 错误码字符串。
// 失败：JSON 损坏、非对象、缺少 code 或 code 非字符串时返回 std::nullopt。
std::optional<std::string> parseResponseCode(std::string_view response_body);

}  // namespace auth
