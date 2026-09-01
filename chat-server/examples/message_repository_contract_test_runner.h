#pragma once

#include "repository/message_repository_contract.h"

#include <functional>
#include <memory>

namespace repository::test
{

// 公共合同测试夹具：Repository 供业务测试使用，cleanup 在连接关闭后清理后端测试资源。
struct RepositoryFixture
{
    std::unique_ptr<IMessageRepository> repository;
    std::function<bool()> cleanup;
};

// 每次调用 Factory 都必须返回一个独立、干净、已初始化的 Repository。
using RepositoryFactory = std::function<RepositoryFixture()>;

// 使用同一个 Factory 分别执行全部公共业务合同测试。
bool runMessageRepositoryContractTests(const RepositoryFactory &factory);

} // namespace repository::test
