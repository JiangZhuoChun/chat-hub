#pragma once

// 预期业务失败的类型化结果（D211）：Outcome<T> = T 或 Error。
// Error.code 使用 D128 固定的稳定 snake_case 错误码；detail 只作内部诊断，
// 禁止把基础设施错误原文透传到协议响应。

#include <string>
#include <string_view>
#include <variant>

namespace chathub::contracts {

struct Error {
    // D128 固定错误码，必须指向静态字面量；不能引用临时 std::string，避免悬空 string_view。
    std::string_view code;
    std::string detail;     // 内部诊断；客户端按错误码本地化，不展示 detail
};

// 本函数只检查 snake_case 形状；D128 完整错误码集合的注册表留给 M1-3。
inline bool is_snake_case_code(std::string_view code) noexcept
{
    if (code.empty() || code.front() == '_' || code.back() == '_') {
        return false;
    }
    for (const char c : code) {
        const bool ok = c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
        if (!ok) {
            return false;
        }
    }
    return true;
}

template<typename T>
// Outcome<T> 的第一个分支是成功值，第二个分支是预期业务失败。
using Outcome = std::variant<T, Error>;

template<typename T>
bool is_ok(const Outcome<T>& result) noexcept
{
    return std::holds_alternative<T>(result);
}

template<typename T>
const T& value(const Outcome<T>& result)
{
    // 前置条件：is_ok(result) 为 true；否则 std::get 抛出，表示调用方违反使用约定。
    return std::get<T>(result);
}

template<typename T>
const Error& error(const Outcome<T>& result)
{
    // 前置条件：is_ok(result) 为 false；业务失败本身仍以 Error 返回，不用异常表达。
    return std::get<Error>(result);
}

} // namespace chathub::contracts
