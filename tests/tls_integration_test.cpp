// M1-6 Task 3b：真实 TLS 集成 —— asio 服务端 handshake ↔ QSslSocket 客户端。
// 验证：握手成功；证书链失败拒绝；IP 不匹配拒绝；不忽略 SSL 错误（D62）。

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QEventLoop>
#include <QSslError>
#include <QSslSocket>
#include <QString>
#include <QTimer>
#include <asio.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "chathub/client/infrastructure/tls_client.hpp"
#include "chathub/pki/dev_ca.hpp"
#include "chathub/server/transport/tls_context.hpp"
#include "chathub/server/transport/tls_handshake.hpp"

using namespace chathub;
using chathub::server::transport::HandshakeStatus;

constexpr auto kTlsHandshakeTimeout = std::chrono::seconds(5);
constexpr auto kHandshakeResultDeadline = std::chrono::seconds(7);

#define CHECK(expr)                                      \
  do {                                                   \
    if (!(expr)) {                                       \
      std::fprintf(stderr, "CHECK failed: %s\n", #expr); \
      return 1;                                          \
    }                                                    \
  } while (0)

static std::filesystem::path uniqueTestDir() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  return std::filesystem::temp_directory_path() /
         ("chathub-m1-6-tls-int-" + std::to_string(dist(gen)));
}

static bool writeTextFile(const std::filesystem::path& p,
                          const std::string& t) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(t.data(), static_cast<std::streamsize>(t.size()));
  out.close();
  return static_cast<bool>(out);
}

class TestDirectory {
 public:
  explicit TestDirectory(std::filesystem::path path) : path_(std::move(path)) {}
  TestDirectory(const TestDirectory&) = delete;
  TestDirectory& operator=(const TestDirectory&) = delete;

  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }

  bool removeNow() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    return !ec;
  }

 private:
  std::filesystem::path path_;
};

// —— 服务端：io_context + acceptor，接受连接并 handshake，结果入队 ——
struct ServerState {
  asio::io_context io;
  std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
  chathub::server::transport::TlsContext* tls = nullptr;
  std::mutex m;
  std::vector<HandshakeStatus> results;
};

static void acceptNext(const std::shared_ptr<ServerState>& s) {
  const std::weak_ptr<ServerState> weak_state = s;
  s->acceptor->async_accept(
      [weak_state](asio::error_code ec, asio::ip::tcp::socket sock) {
        const auto state = weak_state.lock();
        if (!state || ec) return;
        chathub::server::transport::asyncServerHandshake(
            std::move(sock), *state->tls,
            [weak_state](HandshakeStatus st,
                std::shared_ptr<chathub::server::transport::SslStream>) {
              const auto state = weak_state.lock();
              if (!state) return;
              std::lock_guard<std::mutex> g(state->m);
              state->results.push_back(st);
            });
        acceptNext(state);
      });
}

// RAII：测试结束（含 CHECK 早退）时停 io 并 join 后台线程，避免 std::thread
// 析构 terminate。
struct IoThreadGuard {
  std::shared_ptr<ServerState> state;
  std::thread thread;
  IoThreadGuard(std::shared_ptr<ServerState> s, std::thread t)
      : state(std::move(s)), thread(std::move(t)) {}
  ~IoThreadGuard() {
    state->io.stop();
    if (thread.joinable()) thread.join();
  }
};

// 等待服务端回调时仍需处理客户端所属线程的 Qt 事件，不能阻塞主线程。
static bool waitForHandshakeResults(const std::shared_ptr<ServerState>& s,
                                    std::size_t expected_count,
                                    std::chrono::milliseconds timeout) {
  QEventLoop loop;
  QTimer poll;
  QTimer deadline;
  poll.setInterval(std::chrono::milliseconds(10));
  deadline.setSingleShot(true);

  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    std::lock_guard<std::mutex> lock(s->m);
    if (s->results.size() >= expected_count) {
      loop.quit();
    }
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { loop.quit(); });

  poll.start();
  deadline.start(timeout);
  loop.exec();

  std::lock_guard<std::mutex> lock(s->m);
  return s->results.size() >= expected_count;
}

// —— 客户端：QSslSocket 连接，返回结果 ——
enum class ClientResult { encrypted, ssl_error, timeout };

struct ClientRun {
  ClientResult result = ClientResult::timeout;
  bool saw_ssl_errors = false;
  std::unique_ptr<QSslSocket> socket;
};

static ClientRun runClient(const std::filesystem::path& ca_file,
                           unsigned short port, const QString& peer_name,
                           bool keep_connected = false) {
  QString error;
  auto config = chathub::client::infrastructure::TlsClientConfig::build(
      QString::fromStdString(ca_file.string()), error);
  if (!config) return {ClientResult::ssl_error, false, nullptr};

  auto socket = std::make_unique<QSslSocket>();
  config->applyTo(*socket);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  ClientResult result = ClientResult::timeout;
  bool saw_ssl_errors = false;

  QObject::connect(socket.get(), &QSslSocket::encrypted, &loop, [&] {
    result = ClientResult::encrypted;
    loop.quit();
  });
  QObject::connect(socket.get(), &QSslSocket::sslErrors, &loop,
                   [&](const QList<QSslError>&) {
                     // 不调用 ignoreSslErrors（D62）。
                     saw_ssl_errors = true;
                   });
  QObject::connect(socket.get(), &QSslSocket::errorOccurred, &loop,
                   [&](QAbstractSocket::SocketError) {
                     result = ClientResult::ssl_error;
                     loop.quit();
                   });
  QObject::connect(&timer, &QTimer::timeout, &loop, [&] { loop.quit(); });

  socket->connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, peer_name);
  timer.start(5000);  // D73：TLS 5 秒
  loop.exec();

  if (keep_connected && result == ClientResult::encrypted) {
    // TLS 1.3 客户端先发出 encrypted；服务端仍可能在处理 ClientFinished。
    // 成功场景须持有连接，直到服务端 async_handshake 回调完成。
    return {result, saw_ssl_errors, std::move(socket)};
  }

  socket->abort();
  return {result, saw_ssl_errors, nullptr};
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  // 证书：SAN = 127.0.0.1。
  const auto issued = pki::issueDevCa("127.0.0.1");
  CHECK(issued.has_value());
  const auto other_ca = pki::issueDevCa("10.0.0.1");
  CHECK(other_ca.has_value());

  TestDirectory test_dir(uniqueTestDir());
  const auto& dir = test_dir.path();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  CHECK(!ec);
  const auto trusted_ca = dir / "trusted-ca.pem";
  const auto other_ca_file = dir / "other-ca.pem";
  const auto server_cert = dir / "server-cert.pem";
  const auto server_key = dir / "server-key.pem";
  CHECK(writeTextFile(trusted_ca, issued->ca_cert_pem));
  CHECK(writeTextFile(other_ca_file, other_ca->ca_cert_pem));
  CHECK(writeTextFile(server_cert, issued->server_cert_pem));
  CHECK(writeTextFile(server_key, issued->server_key_pem));

  // 服务端：加载证书 + 启动 accept。
  std::string load_err;
  auto tls = chathub::server::transport::TlsContext::load(server_cert,
                                                          server_key, load_err);
  CHECK(tls.has_value());

  auto s = std::make_shared<ServerState>();
  s->tls = &*tls;
  s->acceptor = std::make_unique<asio::ip::tcp::acceptor>(
      s->io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
  const unsigned short port = s->acceptor->local_endpoint().port();
  acceptNext(s);
  IoThreadGuard server_guard = {s, std::thread([s] { s->io.run(); })};

  // 场景 1：正确 CA + 正确 IP → 握手成功。
  auto r1 = runClient(trusted_ca, port, QStringLiteral("127.0.0.1"), true);
  CHECK(r1.result == ClientResult::encrypted);
  CHECK(r1.socket != nullptr);

  // 服务端第一个 handshake 应 ok。
  CHECK(waitForHandshakeResults(s, 1, kTlsHandshakeTimeout));
  {
    std::lock_guard<std::mutex> lock(s->m);
    CHECK(s->results.front() == HandshakeStatus::ok);
  }
  r1.socket->abort();
  r1.socket.reset();

  // 场景 2：错误 CA → 证书链失败拒绝。
  const ClientRun r2 =
      runClient(other_ca_file, port, QStringLiteral("127.0.0.1"));
  CHECK(r2.result == ClientResult::ssl_error);
  CHECK(r2.saw_ssl_errors);
  CHECK(waitForHandshakeResults(s, 2, kTlsHandshakeTimeout));
  {
    std::lock_guard<std::mutex> lock(s->m);
    CHECK(s->results.at(1) == HandshakeStatus::error);
  }

  // 场景 3：正确 CA + 错误 IP 校验名 → IP 不匹配拒绝。
  const ClientRun r3 =
      runClient(trusted_ca, port, QStringLiteral("127.0.0.2"));
  CHECK(r3.result == ClientResult::ssl_error);
  CHECK(r3.saw_ssl_errors);
  CHECK(waitForHandshakeResults(s, 3, kTlsHandshakeTimeout));
  {
    std::lock_guard<std::mutex> lock(s->m);
    CHECK(s->results.at(2) == HandshakeStatus::error);
  }

  // 场景 4：只建立 TCP 连接但不发送 ClientHello → 服务端 TLS 5 秒超时。
  asio::io_context silent_client_io;
  asio::ip::tcp::socket silent_client(silent_client_io);
  const auto timeout_started = std::chrono::steady_clock::now();
  silent_client.connect(
      asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
  CHECK(waitForHandshakeResults(s, 4, kHandshakeResultDeadline));
  const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_started;
  CHECK(timeout_elapsed >= std::chrono::seconds(4));
  CHECK(timeout_elapsed <= kHandshakeResultDeadline);
  {
    std::lock_guard<std::mutex> lock(s->m);
    CHECK(s->results.at(3) == HandshakeStatus::timeout);
  }
  asio::error_code silent_close_ec;
  silent_client.close(silent_close_ec);

  CHECK(test_dir.removeNow());
  return 0;
}