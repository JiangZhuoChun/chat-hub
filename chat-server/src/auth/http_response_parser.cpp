#include "auth/http_response_parser.h"

#include <boost/json/parse.hpp>
#include <charconv>

namespace auth {

// 功能：先验证 "HTTP/" 前缀，再取两个空格之间的纯数字作为三位状态码。
std::optional<int> parseHttpStatusCode(std::string_view status_line) {
  if (status_line.rfind("HTTP/", 0) != 0) {
    return std::nullopt;
  }
  const auto first_space = status_line.find(' ');
  const auto second_space = status_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos ||
      second_space == std::string_view::npos) {
    return std::nullopt;
  }

  int status_code = 0;
  const auto [end, error] =
      std::from_chars(status_line.data() + first_space + 1,
                      status_line.data() + second_space, status_code);

  if (error != std::errc{} || end != status_line.data() + second_space ||
      status_code < 100 || status_code > 599) {
    return std::nullopt;
  }
  return status_code;
}
// 功能：校验 200 成功响应的 JSON：active 必须为 true，username 必须为非空字符串。
std::optional<std::string> parseActiveUsername(std::string_view response_body) {
  boost::system::error_code parse_error;
  const auto json_value = boost::json::parse(response_body, parse_error);

  if (parse_error || !json_value.is_object()) {
    return std::nullopt;
  }

  const auto &object = json_value.as_object();
  const auto *active_value = object.if_contains("active");
  const auto *username_value = object.if_contains("username");

  if (active_value == nullptr || username_value == nullptr ||
      !active_value->is_bool() || !active_value->as_bool() ||
      !username_value->is_string()) {
    return std::nullopt;
  }

  const auto &username = username_value->as_string();
  if (username.empty()) {
    return std::nullopt;
  }

  return std::string(username.data(), username.size());
}
// 功能：解析错误响应 JSON 中的 code 字符串；JSON 损坏或非对象直接视为格式错误。
std::optional<std::string> parseResponseCode(std::string_view response_body) {
  boost::system::error_code parse_error;
  const auto json_value = boost::json::parse(response_body, parse_error);

  if (parse_error || !json_value.is_object()) {
    return std::nullopt;
  }

  const auto *code_value = json_value.as_object().if_contains("code");
  if (code_value == nullptr || !code_value->is_string()) {
    return std::nullopt;
  }

  const auto &code = code_value->as_string();
  return std::string(code.data(), code.size());
}
// 功能：扫描头部区取恰好一次的 Content-Length；缺失、重复、超限或格式损坏都返回 std::nullopt。
std::optional<std::size_t> parseContentLength(
    std::string_view response_headers, std::size_t max_body_bytes) noexcept {
  // Content-Length 的字段名不区分 ASCII 大小写；统一使用小写常量比较。
  constexpr std::string_view CONTENT_LENGTH_HEADER_NAME = "content-length";

  // HTTP header value 两侧允许出现 SP 或 HTAB，但中间不能夹杂空白。
  const auto trimOptionalWhitespace = [](std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    return value;
  };

  const auto equalsIgnoreCase = [](std::string_view left,
                                   std::string_view right) noexcept {
    if (left.size() != right.size()) {
      return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
      const auto toLowerAscii = [](char value) noexcept {
        if (value >= 'A' && value <= 'Z') {
          return static_cast<char>(value - 'A' + 'a');
        }
        return value;
      };

      if (toLowerAscii(left[index]) != toLowerAscii(right[index])) {
        return false;
      }
    }
    return true;
  };

  bool content_length_found = false;
  std::size_t content_length = 0;
  bool first_line = true;

  while (!response_headers.empty()) {
    // 1.按 \r\n 分割一行；最后一行可以没有结尾的 \r\n。
    const auto crlf = response_headers.find("\r\n");
    const std::string_view line = (crlf != std::string_view::npos)
                                      ? response_headers.substr(0, crlf)
                                      : response_headers;

    // 空行表示 header 结束；函数调用方通常会传入分隔符之前的内容。
    if (line.empty()) {
      break;
    }

    // 允许调用方把 HTTP 状态行一起传入，但只允许第一行承担状态行角色；
    // 状态码本身仍由 parseHttpStatusCode() 单独校验。
    const bool is_status_line = first_line && line.rfind("HTTP/", 0) == 0;
    if (!is_status_line) {
      // 2.找到字段名和值之间的冒号。
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) {
        // 非状态行却没有冒号，说明 header 格式损坏，不能继续猜测 body 边界。
        return std::nullopt;
      }

      // 3.只处理 Content-Length，其它合法 header 不影响 body 长度。
      const auto header_name = trimOptionalWhitespace(line.substr(0, colon));
      if (equalsIgnoreCase(header_name, CONTENT_LENGTH_HEADER_NAME)) {
        // 4.Content-Length 必须恰好出现一次，重复字段直接拒绝。
        if (content_length_found) {
          return std::nullopt;
        }

        const auto header_value =
            trimOptionalWhitespace(line.substr(colon + 1));
        if (header_value.empty()) {
          return std::nullopt;
        }

        // 5.只接受完整的十进制无符号整数；from_chars 不接受前后残留字符。
        std::uint64_t parsed_length = 0;
        const auto [end, error] = std::from_chars(
            header_value.data(), header_value.data() + header_value.size(),
            parsed_length);
        if (error != std::errc{} ||
            end != header_value.data() + header_value.size()) {
          return std::nullopt;
        }

        // 6.先检查能否安全转换成 size_t，再执行业务上限检查。
        if (parsed_length > std::numeric_limits<std::size_t>::max() ||
            parsed_length > max_body_bytes) {
          return std::nullopt;
        }

        content_length = static_cast<std::size_t>(parsed_length);
        content_length_found = true;
      }
    }

    // 7.移动到下一行，避免重复处理当前内容。
    if (crlf == std::string_view::npos) {
      break;
    }
    first_line = false;
    response_headers.remove_prefix(crlf + 2);
  }

  // 8.缺少 Content-Length 时，无法确认 body 是否完整，按依赖故障处理。
  if (!content_length_found) {
    return std::nullopt;
  }
  return content_length;
}
}  // namespace auth
