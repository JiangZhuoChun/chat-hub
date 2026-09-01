#include "auth/asio_auth_introspection_client.h"

#include <asio.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using auth::AuthIntrospectionConfig;
using auth::AsioAuthIntrospectionClient;
using auth::IntrospectionRequestPtr;
using auth::IntrospectionResult;
using auth::IntrospectionStatus;
using asio::ip::tcp;
using namespace std::chrono_literals;

std::string makeResponse(int status_code, std::string_view body) {
  const std::string reason = status_code == 200   ? "OK"
                             : status_code == 401 ? "Unauthorized"
                             : status_code == 503 ? "Service Unavailable"
                                                  : "Bad Request";
  return "HTTP/1.1 " + std::to_string(status_code) + " " + reason +
         "\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) +
         "\r\nConnection: keep-alive\r\n\r\n" + std::string(body);
}

class StubAuthHttpServer {
 public:
  StubAuthHttpServer(asio::io_context &io_context, std::string response,
                     bool split_response, bool close_after_write,
                     bool respond)
      : acceptor_(io_context, tcp::endpoint(tcp::v4(), 0)),
        socket_(io_context),
        response_(std::move(response)),
        split_response_(split_response),
        close_after_write_(close_after_write),
        respond_(respond) {}

  ~StubAuthHttpServer() {
    std::error_code ignore_error;
    socket_.close(ignore_error);
    acceptor_.close(ignore_error);
  }

  std::uint16_t port() const {
    return acceptor_.local_endpoint().port();
  }

  const std::string &request_text() const { return request_text_; }

  void start() {
    acceptor_.async_accept(socket_, [this](const std::error_code &error) {
      if (!error) {
        readRequest();
      }
    });
  }

 private:
  bool requestComplete() const {
    const auto separator = request_text_.find("\r\n\r\n");
    if (separator == std::string::npos) {
      return false;
    }

    const auto header_end = separator;
    const auto content_length_pos = request_text_.find("Content-Length:");
    if (content_length_pos == std::string::npos ||
        content_length_pos > header_end) {
      return false;
    }

    const auto value_begin = content_length_pos + std::string_view("Content-Length:").size();
    const auto line_end = request_text_.find("\r\n", value_begin);
    if (line_end == std::string::npos || line_end > header_end) {
      return false;
    }

    auto value = std::string_view(request_text_).substr(
        value_begin, line_end - value_begin);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }

    std::size_t body_length = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), body_length);
    if (error != std::errc{} || end != value.data() + value.size()) {
      return false;
    }

    return request_text_.size() >= separator + 4 + body_length;
  }

  void readRequest() {
    socket_.async_read_some(
        asio::buffer(read_buffer_),
        [this](const std::error_code &error, std::size_t bytes_transferred) {
          if (error) {
            return;
          }

          request_text_.append(read_buffer_.data(), bytes_transferred);
          if (!requestComplete()) {
            readRequest();
            return;
          }

          if (respond_) {
            writeResponse();
          }
        });
  }

  void closeSocket() {
    std::error_code ignore_error;
    socket_.shutdown(tcp::socket::shutdown_both, ignore_error);
    socket_.close(ignore_error);
  }

  void writeResponse() {
    if (!split_response_) {
      asio::async_write(
          socket_, asio::buffer(response_),
          [this](const std::error_code &error, std::size_t) {
            if (!error && close_after_write_) {
              closeSocket();
            }
          });
      return;
    }

    const auto separator = response_.find("\r\n\r\n");
    response_header_ = response_.substr(0, separator + 4);
    response_body_ = response_.substr(separator + 4);
    asio::async_write(
        socket_, asio::buffer(response_header_),
        [this](const std::error_code &error, std::size_t) {
          if (error) {
            return;
          }
          asio::async_write(
              socket_, asio::buffer(response_body_),
              [this](const std::error_code &body_error, std::size_t) {
                if (!body_error && close_after_write_) {
                  closeSocket();
                }
              });
        });
  }

  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::array<char, 4096> read_buffer_{};
  std::string request_text_;
  std::string response_;
  std::string response_header_;
  std::string response_body_;
  bool split_response_{false};
  bool close_after_write_{false};
  bool respond_{true};
};

struct ScenarioResult {
  std::optional<IntrospectionResult> result;
  std::string request_text;
};

ScenarioResult runScenario(std::string response, bool split_response,
                           bool close_after_write, bool respond,
                           std::chrono::milliseconds timeout,
                           bool cancel_request = false) {
  asio::io_context io_context;
  StubAuthHttpServer stub(io_context, std::move(response), split_response,
                          close_after_write, respond);
  stub.start();

  AuthIntrospectionConfig config;
  config.host = "127.0.0.1";
  config.port = std::to_string(stub.port());
  config.target = "/internal/auth/introspect";
  config.internal_service_key = "test-internal-key";
  config.timeout = timeout;
  config.max_response_body_bytes = 4096;

  AsioAuthIntrospectionClient client(io_context, config);
  std::optional<IntrospectionResult> result;
  const IntrospectionRequestPtr request = client.introspect(
      "token-value", [&](IntrospectionResult received) {
        result = std::move(received);
        io_context.stop();
      });

  if (cancel_request) {
    asio::steady_timer cancel_timer(io_context, 20ms);
    cancel_timer.async_wait([request](const std::error_code &error) {
      if (!error) {
        request->cancel();
      }
    });
    asio::steady_timer stop_timer(io_context, 100ms);
    stop_timer.async_wait([&io_context](const std::error_code &) {
      io_context.stop();
    });
    io_context.run();
  } else {
    io_context.run();
  }

  return {std::move(result), stub.request_text()};
}

bool isStatus(const ScenarioResult &scenario, IntrospectionStatus status,
              std::string_view username = {}, std::string_view error_code = {}) {
  if (!scenario.result.has_value() || scenario.result->status != status) {
    return false;
  }
  return scenario.result->username == username &&
         scenario.result->error_code == error_code;
}

bool testActiveResponseAndRequestContract() {
  const auto scenario = runScenario(
      makeResponse(200, R"({"active":true,"username":"alice"})"), true,
      false, true, 500ms);
  return isStatus(scenario, IntrospectionStatus::active, "alice") &&
         scenario.request_text.find(
             "POST /internal/auth/introspect HTTP/1.1\r\n") !=
             std::string::npos &&
         scenario.request_text.find(
             "X-Internal-Service-Key: test-internal-key\r\n") !=
             std::string::npos &&
         scenario.request_text.find(R"({"token":"token-value"})") !=
             std::string::npos;
}

bool testAuthenticationAndDependencyStatuses() {
  const auto rejected = runScenario(
      makeResponse(401, R"({"code":"authentication_rejected"})"), false,
      true, true, 500ms);
  const auto service_rejected = runScenario(
      makeResponse(401, R"({"code":"internal_service_rejected"})"), false,
      true, true, 500ms);
  const auto unavailable =
      runScenario(makeResponse(503, R"({"error":"unavailable"})"), false,
                   true, true, 500ms);

  return isStatus(rejected, IntrospectionStatus::authentication_rejected) &&
         isStatus(service_rejected, IntrospectionStatus::dependency_unavailable,
                  {}, "service_rejected") &&
         isStatus(unavailable, IntrospectionStatus::dependency_unavailable, {},
                  "service_unavailable");
}

bool testMalformedFramingAndBodyStatuses() {
  const auto malformed_body = runScenario(
      makeResponse(200, R"({"active":true})"), false, true, true, 500ms);

  const std::string duplicate_content_length =
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n"
      "Connection: close\r\n\r\n{}";
  const auto duplicate_length =
      runScenario(duplicate_content_length, false, true, true, 500ms);

  const std::string truncated =
      "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\n{}";
  const auto body_truncated =
      runScenario(truncated, false, true, true, 500ms);

  const std::string oversized_length =
      "HTTP/1.1 200 OK\r\nContent-Length: 4097\r\nConnection: close\r\n\r\n";
  const auto oversized =
      runScenario(oversized_length, false, true, true, 500ms);

  return isStatus(malformed_body, IntrospectionStatus::dependency_unavailable,
                  {}, "malformed_response_body") &&
         isStatus(duplicate_length, IntrospectionStatus::dependency_unavailable,
                  {}, "invalid_content_length") &&
         isStatus(body_truncated, IntrospectionStatus::dependency_unavailable,
                  {}, "body_truncated") &&
         isStatus(oversized, IntrospectionStatus::dependency_unavailable, {},
                  "invalid_content_length");
}

bool testTimeoutAndCancellation() {
  const auto timeout = runScenario({}, false, false, false, 50ms);
  const auto cancelled = runScenario({}, false, false, false, 500ms, true);

  return isStatus(timeout, IntrospectionStatus::dependency_unavailable, {},
                  "timeout") &&
         !cancelled.result.has_value();
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
  const bool active_passed = runTest(
      "当 Auth 返回有效身份时，introspection 客户端应完成请求并返回 active",
      testActiveResponseAndRequestContract());
  const bool status_passed = runTest(
      "当 Auth 返回认证拒绝或依赖故障时，introspection 客户端应映射对应状态",
      testAuthenticationAndDependencyStatuses());
  const bool framing_passed = runTest(
      "当 Auth 响应分帧或正文非法时，introspection 客户端应拒绝半成品",
      testMalformedFramingAndBodyStatuses());
  const bool lifecycle_passed =
      runTest("当 introspection 超时或被取消时，请求应在截止时间内结束",
              testTimeoutAndCancellation());
  return active_passed && status_passed && framing_passed && lifecycle_passed
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
