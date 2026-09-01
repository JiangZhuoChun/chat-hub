#include "auth/http_response_parser.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool expectStatus(std::string_view text, std::optional<int> expected) {
  const auto actual = auth::parseHttpStatusCode(text);
  return actual == expected;
}

bool expectContentLength(std::string_view headers, std::size_t max_bytes,
                         std::optional<std::size_t> expected) {
  const auto actual = auth::parseContentLength(headers, max_bytes);
  return actual == expected;
}

bool testStatusLineContract() {
  return expectStatus("HTTP/1.1 200 OK", 200) &&
         expectStatus("HTTP/1.0 401 Unauthorized", 401) &&
         !expectStatus("HTTP/1.1 20 OK", 20) &&
         !expectStatus("NOTHTTP/1.1 200 OK", 200) &&
         !expectStatus("HTTP/1.1 200", 200) &&
         !expectStatus("HTTP/1.1 200x OK", 200) &&
         !expectStatus("HTTP/1.1 600 Too Large", 600);
}

bool testContentLengthContract() {
  const std::string valid_headers =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
      "Content-Length: 17\r\nConnection: close";

  return expectContentLength(valid_headers, 4096, 17) &&
         expectContentLength("Content-Length: 0", 4096, 0) &&
         expectContentLength("content-length:\t 8  \r\n", 4096, 8) &&
         !expectContentLength("X-Test: one\r\n", 4096, 0) &&
         !expectContentLength("Content-Length: 4\r\nContent-Length: 4", 4096,
                              4) &&
         !expectContentLength("Content-Length: ", 4096, 0) &&
         !expectContentLength("Content-Length: 4x", 4096, 4) &&
         !expectContentLength("Content-Length: +4", 4096, 4) &&
         !expectContentLength("Content-Length: 4097", 4096, 4097) &&
         !expectContentLength("Broken-Header", 4096, 0);
}

bool testJsonBodyContract() {
  const auto active = auth::parseActiveUsername(
      R"({"active":true,"username":"alice"})");
  const auto inactive = auth::parseActiveUsername(
      R"({"active":false,"username":"alice"})");
  const auto empty_username = auth::parseActiveUsername(
      R"({"active":true,"username":""})");
  const auto rejected = auth::parseResponseCode(
      R"({"code":"authentication_rejected"})");
  const auto non_object = auth::parseResponseCode(R"(["code"])");

  return active.has_value() && *active == "alice" && !inactive.has_value() &&
         !empty_username.has_value() && rejected.has_value() &&
         *rejected == "authentication_rejected" && !non_object.has_value();
}

bool runTest(const char *name, bool passed) {
  if (passed) {
    std::cout << "PASS: " << name << '\n';
    return true;
  }
  std::cerr << "FAIL: " << name << '\n';
  return false;
}

}  // namespace

int main() {
  const bool status_passed =
      runTest("当 HTTP 状态行格式合法或非法时，状态码解析应返回对应结果",
              testStatusLineContract());
  const bool content_length_passed = runTest(
      "当响应头包含 Content-Length 时，长度解析应拒绝重复或越界声明",
      testContentLengthContract());
  const bool json_passed =
      runTest("当 introspection 响应正文合法或非法时，JSON 解析应返回对应身份或错误码",
              testJsonBodyContract());
  return status_passed && content_length_passed && json_passed ? EXIT_SUCCESS
                                                               : EXIT_FAILURE;
}
