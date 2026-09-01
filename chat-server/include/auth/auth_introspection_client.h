#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace auth {
struct AuthIntrospectionConfig {
  std::string host;  // Auth Service 主机
  std::string port;  // HTTP 端口，先保存为字符串，便于交给 Asio resolver；
  std::string target{
      "/internal/auth/introspect"};         // HTTP 请求路径，不保存完整 URL；
  std::string internal_service_key;         // 服务凭证，不能记录日志；
  std::chrono::milliseconds timeout{2000};  // 一次 introspection 最大等待时间；
  std::size_t max_response_body_bytes{4096};  // 限制异常响应占用内存。
};
enum class IntrospectionStatus {
  active,                   // 本次查询时 token 有效且未撤销
  authentication_rejected,  // 用户 token 无效、过期或已撤销
  dependency_unavailable    // 依赖服务不可用
};

struct IntrospectionResult {
  IntrospectionStatus status{IntrospectionStatus::dependency_unavailable};
  std::string username;
  std::string error_code;  // 仅 dependency_unavailable 时非空
};
using IntrospectionHandler = std::function<void(IntrospectionResult)>;

class IAuthIntrospectionRequest {
 public:
  virtual ~IAuthIntrospectionRequest() = default;
  // 可从任意线程调用，具体实现必须切回请求自己的 strand。
  virtual void cancel() = 0;
};
using IntrospectionRequestPtr = std::shared_ptr<IAuthIntrospectionRequest>;

class IAuthIntrospectionClient {
 public:
  virtual ~IAuthIntrospectionClient() = default;
  // 因为 HTTP 是异步的，调用函数返回后，原来的协议帧对象可能已经销毁；
  // 客户端必须拥有自己的 token 副本。
  virtual IntrospectionRequestPtr introspect(
      std::string token, IntrospectionHandler handler) = 0;
};

}  // namespace auth