#include "auth/auth_introspection_client.h"
#include "auth_introspection_config.h"
#include "message_repository_factory.h"
#include "server_runtime_config.h"

#include <asio.hpp>
#include <net/server.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <cstddef>
#include <vector>

namespace
{
constexpr std::size_t kWorkerThreadCount = 2;

// 功能：统一拥有 Asio 工作线程；无论正常等待还是启动异常，
// 都保证已经创建的线程会在对象销毁前被停止并回收。
class WorkerThreadGroup
{
public:
    // io_context 的所有权仍属于 main()；本类只借用它来运行和停止事件循环。
    explicit WorkerThreadGroup(asio::io_context& io_context) noexcept :
    m_io_context(io_context)
    {}

    ~WorkerThreadGroup() noexcept
    {
        m_io_context.stop();
        joinAll();
    }
    WorkerThreadGroup(const WorkerThreadGroup&) = delete;
    WorkerThreadGroup& operator=(const WorkerThreadGroup&) = delete;

    // 功能：创建指定数量的工作线程，每个线程都运行同一个 io_context。
    // 失败：若中途创建线程抛异常，析构函数会停止并回收已经成功创建的线程。
    void start(const std::size_t worker_count)
    {
        m_workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            m_workers.emplace_back([this] {
                m_io_context.run();
            });
        }
    }
    // 功能：等待并回收全部仍有效的工作线程。
    // joinable() 防止对已回收或从未成功创建的线程再次调用 join()。
    void joinAll()
    {
        for (std::thread& worker : m_workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

private:
    asio::io_context& m_io_context;
    std::vector<std::thread> m_workers;
};

bool isConfigurationError(const app::MessageRepositoryStartupError error) noexcept
{
    return error == app::MessageRepositoryStartupError::MissingMySqlPassword ||
           error == app::MessageRepositoryStartupError::MySqlInvalidConfig;
}
}

// ==================== 模块：聊天服务器启动入口 ====================
// 功能：创建 Asio 事件循环和聊天服务器，并由两个工作线程持续处理网络事件。
// 失败：端口绑定等同步初始化异常时输出原因并以失败状态退出。
int main(int argc, char *argv[])
{
    const auto result = app::parseServerRuntimeConfig(argc, argv);
    if (!result.config)
    {
        std::cerr << "configuration_error: " << app::serverRuntimeConfigErrorCode(result.error) << std::endl;
        return EXIT_FAILURE;
    }
    const auto &config = *result.config;

    auto auth_config_result = app::loadAuthIntrospectionConfigFromEnvironment();
    if (!auth_config_result.config)
    {
        std::cerr << "configuration_error: "
                  << app::authIntrospectionConfigErrorCode(auth_config_result.error)
                  << std::endl;
        return EXIT_FAILURE;
    }
    auto auth_introspection_config = std::move(*auth_config_result.config);

    auto factory_result = app::createMessageRepository(config);
    if (!factory_result.repository)
    {
        const char* const prefix = isConfigurationError(factory_result.error)
        ? "configuration_error: "
        : "repository_startup_error: ";
        std::cerr << prefix << app::messageRepositoryStartupErrorCode(factory_result.error)
                  << std::endl;
        return EXIT_FAILURE;
    }


    try
    {
        asio::io_context io_context;
        net::Server server(io_context, config.port,
                            config.database_path.string(),
                             config.authentication_timeout,
                             std::move(auth_introspection_config),
                             std::move(factory_result.repository));
        server.start();

        // 必须在 server 之后创建：析构时线程先停止并回收，随后才销毁 server
        WorkerThreadGroup workers(io_context);
        workers.start(kWorkerThreadCount);
        workers.joinAll();
    }
    catch (const std::exception &error)
    {
        std::cerr << "服务器启动失败：" << error.what() << std::endl;
        return 1;
    }

    return 0;
}
