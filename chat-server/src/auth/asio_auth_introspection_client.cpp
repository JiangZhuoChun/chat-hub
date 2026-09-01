#include "auth/asio_auth_introspection_client.h"

#include <array>
#include <boost/json.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "auth/http_response_parser.h"

namespace {
using namespace auth;

enum class IntrospectionErrorCode {
  none,            // 默认值：非故障（active / authentication_rejected 用它）
  resolve_failed,  // DNS 解析失败
  connect_failed,  // TCP 连接失败
  write_failed,    // 请求写失败
  timeout,         // 总超时
  read_failed,     // 读中断（非 EOF 的错误）
  response_too_large,       // 响应超过容量上限（两处检查共用）
  invalid_content_length,   // 缺/重复/超长 Content-Length
  malformed_status_line,    // 状态行不是合法 HTTP/1.x 200
  service_unavailable,      // 503
  unexpected_http_status,   // 400/500 等其余非 200/401 状态
  body_truncated,           // EOF 早于 Content-Length 承诺的字节数
  malformed_response_body,  // 200 但 JSON 坏 / 缺 active / username 空
  service_rejected  // 401 且 code=internal_service_rejected（密钥/配置故障）
};

std::string_view introspectionErrorCodeString(
    IntrospectionErrorCode code) noexcept {
  switch (code) {
    case IntrospectionErrorCode::none:
      return "";
    case IntrospectionErrorCode::resolve_failed:
      return "resolve_failed";
    case IntrospectionErrorCode::connect_failed:
      return "connect_failed";
    case IntrospectionErrorCode::write_failed:
      return "write_failed";
    case IntrospectionErrorCode::timeout:
      return "timeout";
    case IntrospectionErrorCode::read_failed:
      return "read_failed";
    case IntrospectionErrorCode::response_too_large:
      return "response_too_large";
    case IntrospectionErrorCode::invalid_content_length:
      return "invalid_content_length";
    case IntrospectionErrorCode::malformed_status_line:
      return "malformed_status_line";
    case IntrospectionErrorCode::service_unavailable:
      return "service_unavailable";
    case IntrospectionErrorCode::unexpected_http_status:
      return "unexpected_http_status";
    case IntrospectionErrorCode::body_truncated:
      return "body_truncated";
    case IntrospectionErrorCode::malformed_response_body:
      return "malformed_response_body";
    case IntrospectionErrorCode::service_rejected:
      return "service_rejected";
    default:
      return "unknown_error_code";
  }
}

// 功能：构造"依赖不可用"结果并附上具体失败阶段码；非故障结果不经过它。
IntrospectionResult makeDependencyUnavailable(IntrospectionErrorCode code) {
  return IntrospectionResult{IntrospectionStatus::dependency_unavailable,
                             {},
                             std::string(introspectionErrorCodeString(code))};
}

class RequestOperation : public auth::IAuthIntrospectionRequest,
                         public std::enable_shared_from_this<RequestOperation> {
 public:
  RequestOperation(asio::io_context &io_context, AuthIntrospectionConfig config,
                   std::string token, IntrospectionHandler handler)
      : strand(io_context.get_executor()),
        resolver(io_context),
        socket(io_context),
        timer(io_context),
        config(std::move(config)),
        token(std::move(token)),
        handler(std::move(handler)),
        finished(false),
        read_chunk() {}
  // ---- 核心流程函数 ----
  void start();
  void cancel() override;
  void cancelOnStrand();
  void startOnStrand();
  void buildRequest();
  void handleResolve(const std::error_code &error,
                     const asio::ip::tcp::resolver::results_type &endpoints);
  void handleConnect(const std::error_code &error,
                     const asio::ip::tcp::endpoint &endpoint);
  void handleWrite(const std::error_code &error, std::size_t bytes_transferred);
  void handleRead(const std::error_code &error, std::size_t bytes_transferred);
  void handleCompleteResponseBody();
  void handleTimeout(const std::error_code &error);
  void finish(IntrospectionResult result);

 private:
  // ---- 网络与定时资源（每请求独立） ----
  asio::strand<asio::any_io_executor> strand;
  asio::ip::tcp::resolver resolver;  // DNS/地址解析
  asio::ip::tcp::socket socket;      // TCP套接字
  asio::steady_timer timer;          // 定时器
  // ---- 请求数据 ----
  AuthIntrospectionConfig config;  // 当前请求所用配置
  std::string token;               // 拥有 token 副本
  std::string request_text;        // 拥有完整 HTTP 请求
  IntrospectionHandler handler;    // 最终回调
  // ---- 状态与缓冲 ----
  bool finished;  // 防止重复回调
  bool cancelled{false};
  std::array<char, 1024> read_chunk;  // 临时读取缓冲区
  std::string response_text;          // 累积响应内容

  bool response_headers_parsed{false};
  int response_status_code{0};

  std::size_t response_body_bytes_expected{0};
  bool response_content_length_parsed{false};
};

// ----------------------------------------------------------------------------
// start(): 设置超时 → 构造请求 → 发起 DNS 解析
// ----------------------------------------------------------------------------

void RequestOperation::start() {
  auto self = shared_from_this();

  asio::post(strand, [self] { self->startOnStrand(); });
}
void RequestOperation::cancel() {
  auto self = shared_from_this();
  asio::post(strand, [self] { self->cancelOnStrand(); });
}
void RequestOperation::cancelOnStrand() {
  if (finished) {
    return;
  }
  cancelled = true;
  std::error_code ignore_ec;
  timer.cancel(ignore_ec);
  resolver.cancel();
  socket.cancel(ignore_ec);

  // 移出 handler，避免在回调中持有自身引用
  finish(makeDependencyUnavailable(IntrospectionErrorCode::none));
}
void RequestOperation::startOnStrand() {
  auto self = shared_from_this();
  // 1. 设置总超时计时器（从 start 开始计算，覆盖 DNS + 连接 + 读写）
  timer.expires_after(config.timeout);
  timer.async_wait(asio::bind_executor(
      strand, [self](const std::error_code &ec) { self->handleTimeout(ec); }));

  // 2. 构造完整 HTTP 请求文本（必须在 async_resolve 之前完成）
  buildRequest();

  // 3. 发起异步 DNS 解析
  resolver.async_resolve(
      config.host, config.port,
      asio::bind_executor(
          strand,
          [self](const std::error_code &ec,
                 const asio::ip::tcp::resolver::results_type &endpoints) {
            self->handleResolve(ec, endpoints);
          }));
}
void RequestOperation::buildRequest() {
  boost::json::object request_object;
  request_object["token"] = token;
  const std::string body = boost::json::serialize(request_object);

  std::ostringstream oss;
  oss << "POST " << config.target << " HTTP/1.1\r\n"
      << "Host: " << config.host << ":" << config.port << "\r\n"
      << "X-Internal-Service-Key: " << config.internal_service_key << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Accept: application/json\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "\r\n"
      << body;

  request_text = oss.str();
}
void RequestOperation::handleResolve(
    const std::error_code &error,
    const asio::ip::tcp::resolver::results_type &endpoints) {
  if (finished) {
    return;
  }
  if (error) {
    // DNS 解析失败
    finish(makeDependencyUnavailable(IntrospectionErrorCode::resolve_failed));
    return;
  }

  auto self = shared_from_this();
  asio::async_connect(
      socket, endpoints,
      asio::bind_executor(strand, [self](const std::error_code &ec,
                                         const asio::ip::tcp::endpoint &ep) {
        self->handleConnect(ec, ep);
      }));
}
void RequestOperation::handleConnect(const std::error_code &error,
                                     const asio::ip::tcp::endpoint &endpoint) {
  if (finished) {
    return;
  }
  if (error) {
    finish(makeDependencyUnavailable(IntrospectionErrorCode::connect_failed));
    return;
  }
  auto self = shared_from_this();
  asio::async_write(
      socket, asio::buffer(request_text),
      asio::bind_executor(strand, [self](const std::error_code &ec,
                                         const std::size_t bytes_transferred) {
        self->handleWrite(ec, bytes_transferred);
      }));
}
void RequestOperation::handleWrite(const std::error_code &error,
                                   std::size_t bytes_transferred) {
  if (finished) {
    return;
  }
  if (error) {
    finish(makeDependencyUnavailable(IntrospectionErrorCode::write_failed));
    return;
  }
  auto self = shared_from_this();
  socket.async_read_some(
      asio::buffer(read_chunk),
      asio::bind_executor(
          strand, [self](const std::error_code &ec, const std::size_t n) {
            self->handleRead(ec, n);
          }));
}
void RequestOperation::handleCompleteResponseBody() {
  if (response_status_code == 401) {
    const auto response_code = parseResponseCode(response_text);
    if (response_code && *response_code == "authentication_rejected") {
      finish({IntrospectionStatus::authentication_rejected,
              {}});  // 非故障：error_code 保持空
    } else if (response_code && *response_code == "internal_service_rejected") {
      finish(
          makeDependencyUnavailable(IntrospectionErrorCode::service_rejected));
    } else {
      // 401 但 code 未知或正文格式坏：合同漂移，按依赖故障处理。
      finish(makeDependencyUnavailable(
          IntrospectionErrorCode::malformed_response_body));
    }
    return;
  }

  if (response_status_code != 200) {
    // 防御性分支：handleRead 已拒绝非 200/401，正常不可达，仍填码。
    finish(makeDependencyUnavailable(
        IntrospectionErrorCode::unexpected_http_status));
    return;
  }

  const auto username = parseActiveUsername(response_text);
  if (!username) {
    finish(makeDependencyUnavailable(
        IntrospectionErrorCode::malformed_response_body));
    return;
  }
  finish({IntrospectionStatus::active, *username});
}
void RequestOperation::handleRead(const std::error_code &error,
                                  const std::size_t bytes_transferred) {
  if (finished) {
    return;
  }
  if (error && error != asio::error::eof) {  // 非 EOF 读错误
    finish(makeDependencyUnavailable(IntrospectionErrorCode::read_failed));
    return;
  }
  if (bytes_transferred > 0) {
    // 累积已读数据，继续读取剩余部分
    response_text.append(read_chunk.data(), bytes_transferred);
  }
  if (response_text.size() > config.max_response_body_bytes) {  // 头+体超 4096
    finish(
        makeDependencyUnavailable(IntrospectionErrorCode::response_too_large));
    return;
  }
  if (!response_headers_parsed) {
    if (const auto separator = response_text.find("\r\n\r\n");
        separator != std::string::npos) {
      // 1.先在删除 header 之前解析 Content-Length；
      //  response_text仍然同时包含状态行、header 和已经收到的 body。
      const auto response_headers =
          std::string_view(response_text).substr(0, separator);
      const auto content_length =
          parseContentLength(response_headers, config.max_response_body_bytes);
      if (!content_length) {
        // 无法确定 body 边界时，不能把当前内容交给 JSON 解析器。
        finish(makeDependencyUnavailable(
            IntrospectionErrorCode::invalid_content_length));
        return;
      }

      response_body_bytes_expected = *content_length;
      response_content_length_parsed = true;

      const auto first_line_end = response_text.find("\r\n");

      if (first_line_end ==
          std::string::npos) {  // 首行无 CRLF（防御性，实际不可达）
        finish(makeDependencyUnavailable(
            IntrospectionErrorCode::malformed_status_line));
        return;
      }

      const auto status_line =
          std::string_view(response_text).substr(0, first_line_end);
      const auto status_code = parseHttpStatusCode(status_line);
      if (!status_code) {  // 状态行不是合法 HTTP
        finish(makeDependencyUnavailable(
            IntrospectionErrorCode::malformed_status_line));
        return;
      }
      response_status_code = *status_code;
      response_headers_parsed = true;

      // 2.去掉状态行和 header 分隔符；从这里开始 response_text 只表示 body。
      response_text.erase(0, separator + 4);

      if (response_status_code == 503) {
        finish(makeDependencyUnavailable(
            IntrospectionErrorCode::service_unavailable));
        return;
      }

      if (response_status_code != 200 && response_status_code != 401) {
        finish(makeDependencyUnavailable(
            IntrospectionErrorCode::unexpected_http_status));
        return;
      }
    }
  }

  // 3.这个判断必须放在 header 分支之外，因为后续 read 也可能带来超长 body。
  if (response_headers_parsed && response_content_length_parsed &&
      response_text.size() > response_body_bytes_expected) {
    // body 超过 CL 承诺
    finish(
        makeDependencyUnavailable(IntrospectionErrorCode::response_too_large));
    return;
  }

  // 4.Content-Length 已经给出明确边界；达到精确长度即可完成本次响应，
  //    不需要等待对端额外发送 EOF。
  if (response_headers_parsed && response_content_length_parsed &&
      response_text.size() == response_body_bytes_expected) {
    handleCompleteResponseBody();
    return;
  }

  if (error == asio::error::eof) {
    finish(makeDependencyUnavailable(IntrospectionErrorCode::body_truncated));
    return;
  }
  auto self = shared_from_this();
  socket.async_read_some(
      asio::buffer(read_chunk),
      asio::bind_executor(
          strand, [self](const std::error_code &ec, const std::size_t n) {
            self->handleRead(ec, n);
          }));
}
void RequestOperation::handleTimeout(const std::error_code &error) {
  // 如果已被正常 finish，timer 被 cancel 后会收到 operation_aborted
  if (finished || error == asio::error::operation_aborted) {
    return;
  }
  finish(makeDependencyUnavailable(IntrospectionErrorCode::timeout));
}
// ----------------------------------------------------------------------------
// finish(): 统一终结路径，防止重复回调
// ----------------------------------------------------------------------------
void RequestOperation::finish(IntrospectionResult result) {
  if (finished) {
    return;
  }
  finished = true;

  // 取消所有未完成的异步操作
  std::error_code ignore_ec;
  timer.cancel(ignore_ec);
  resolver.cancel();
  socket.close(ignore_ec);

  if (cancelled) {
    handler = {};
    return;
  }

  if (result.status == IntrospectionStatus::dependency_unavailable &&
      !result.error_code.empty()) {
    std::ostringstream oss;
    oss << "phase=auth-introspection event=dependency_unavailable code="
        << result.error_code;

    static std::mutex log_mutex;
    {
      std::lock_guard<std::mutex> lock(log_mutex);
      std::cout << oss.str() << '\n';
    }
  }
  // 移出 handler 并调用，避免在回调中持有自身引用
  if (const auto cb = std::move(handler)) {
    cb(std::move(result));
  }
}
}  // namespace

namespace auth {
// ============================================================================
// AsioAuthIntrospectionClient 公开接口实现
// ============================================================================

AsioAuthIntrospectionClient::AsioAuthIntrospectionClient(
    asio::io_context &io_context, AuthIntrospectionConfig config)
    : m_io_context(io_context), m_config(std::move(config))
{}

IntrospectionRequestPtr AsioAuthIntrospectionClient::introspect(
    std::string token, IntrospectionHandler handler) {
  // 每次请求创建独立的 RequestOperation，资源不共享
  const auto operation = std::make_shared<RequestOperation>(
      m_io_context, m_config, std::move(token), std::move(handler));

  // start() 内部会启动异步链并通过 shared_from_this 维持生命周期
  operation->start();
  return operation;
}
}  // namespace auth
