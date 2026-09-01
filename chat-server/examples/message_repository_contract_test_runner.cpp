#include "message_repository_contract_test_runner.h"

#include <iostream>
#include <optional>

namespace repository::test
{
namespace
{

using ContractTest = bool (*)(IMessageRepository &repository);

// 测试类型：公共合同测试。
// 测试内容：历史查询必须满足用户隔离、limit、稳定排序、has_more、next_cursor 和复合游标翻页合同。
bool testHistoryQueryContract(IMessageRepository &repository)
{
    const auto store_message = [&repository](const char *sender, const char *recipient, const char *local_id,
                                             const char *content, const char *client_send_at, const char *message_id,
                                             const std::int64_t received_at_ms) {
        NewMessage message;
        message.sender = sender;
        message.recipient = recipient;
        message.client_local_id = local_id;
        message.content = content;
        message.client_send_at = client_send_at;
        message.message_id = message_id;
        message.server_received_at_ms = received_at_ms;
        return repository.storeMessage(message).result == StoreResult::Stored;
    };

    if (!store_message("Alice", "Bob", "alice-local-001", "message at 1000", "client-time-1000-a",
                       "00000000000000000000000000000001", 1000) ||
        !store_message("Alice", "Bob", "alice-local-002", "first message at 2000", "client-time-2000-b",
                       "00000000000000000000000000000002", 2000) ||
        !store_message("Alice", "Bob", "alice-local-003", "second message at 2000", "client-time-2000-c",
                       "00000000000000000000000000000003", 2000) ||
        !store_message("Bob", "Alice", "bob-local-001", "third message at 2000", "client-time-2000-d",
                       "00000000000000000000000000000004", 2000) ||
        !store_message("Carol", "Dave", "carol-local-001", "unrelated message", "client-time-3000",
                       "00000000000000000000000000000005", 3000))
    {
        std::cerr << "历史合同测试的准备消息写入失败\n";
        return false;
    }

    HistoryQueryResult first_page;
    if (!repository.loadRecentForUser("Alice", std::nullopt, 2, first_page))
    {
        std::cerr << "历史合同测试无法加载第一页\n";
        return false;
    }
    if (first_page.messages.size() != 2)
    {
        std::cerr << "expected 2 first-page messages, got " << first_page.messages.size() << '\n';
        return false;
    }

    // 验证用户隔离：Alice 的历史中不能出现 Carol 与 Dave 的第三方会话。
    for (const auto &message : first_page.messages)
    {
        if (message.sender != "Alice" && message.recipient != "Alice")
        {
            std::cerr << "message unrelated to Alice leaked into history\n";
            return false;
        }
    }

    // 验证首页取最新两条，但页面内部仍按旧到新排序；同毫秒时使用 message_id 稳定排序。
    if (first_page.messages[0].server_received_at_ms != 2000 || first_page.messages[1].server_received_at_ms != 2000 ||
        first_page.messages[0].message_id >= first_page.messages[1].message_id)
    {
        std::cerr << "first-page ordering contract failed\n";
        return false;
    }

    // 验证 has_more 与 next_cursor 一致，游标指向本页最旧的一条消息。
    if (!first_page.has_more || !first_page.next_cursor.has_value() ||
        first_page.next_cursor->server_received_at_ms != first_page.messages.front().server_received_at_ms ||
        first_page.next_cursor->message_id != first_page.messages.front().message_id)
    {
        std::cerr << "first-page cursor contract failed\n";
        return false;
    }

    HistoryQueryResult older_page;
    if (!repository.loadRecentForUser("Alice", first_page.next_cursor, 2, older_page))
    {
        std::cerr << "历史合同测试无法加载第二页\n";
        return false;
    }
    if (older_page.messages.size() != 2)
    {
        std::cerr << "expected 2 older-page messages, got " << older_page.messages.size() << '\n';
        return false;
    }

    // 验证复合游标严格取游标之前的数据，跨页不能重复 message_id。
    for (const auto &first : first_page.messages)
    {
        for (const auto &older : older_page.messages)
        {
            if (first.message_id == older.message_id)
            {
                std::cerr << "duplicate message across history pages\n";
                return false;
            }
        }
    }
    if (older_page.messages[0].server_received_at_ms != 1000 || older_page.messages[1].server_received_at_ms != 2000 ||
        older_page.messages[1].message_id >= first_page.next_cursor->message_id)
    {
        std::cerr << "composite cursor contract failed\n";
        return false;
    }

    return true;
}

// 测试类型：公共合同测试。
// 测试内容：首次写入返回 Stored；完全重复复用原 ID/时间；冲突重复不覆盖原持久记录。
bool testStoreIdempotencyContract(IMessageRepository &repository)
{
    NewMessage first_message;
    first_message.sender = "Alice";
    first_message.recipient = "Bob";
    first_message.client_local_id = "alice-duplicate-001";
    first_message.content = "只应保存一次";
    first_message.client_send_at = "2026-08-17T12:00:00.000Z";
    first_message.server_received_at_ms = 1000;
    first_message.message_id = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    const StoreOutcome first_outcome = repository.storeMessage(first_message);
    if (first_outcome.result != StoreResult::Stored || first_outcome.message_id != first_message.message_id)
    {
        std::cerr << "首次写入没有保存并返回候选 message_id\n";
        return false;
    }

    NewMessage replayed_message = first_message;
    replayed_message.server_received_at_ms = 2000;
    replayed_message.message_id = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    const StoreOutcome replayed_outcome = repository.storeMessage(replayed_message);
    if (replayed_outcome.result != StoreResult::DuplicateSame ||
        replayed_outcome.message_id != first_message.message_id ||
        replayed_outcome.server_received_at_ms != first_outcome.server_received_at_ms)
    {
        std::cerr << "完全重复请求没有复用原持久记录\n";
        return false;
    }

    NewMessage conflicting_message = first_message;
    conflicting_message.content = "同一 local_id 但正文不同";
    conflicting_message.message_id = "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";
    const StoreOutcome conflict_outcome = repository.storeMessage(conflicting_message);
    if (conflict_outcome.result != StoreResult::IdempotencyConflict)
    {
        std::cerr << "冲突重复请求没有返回 IdempotencyConflict\n";
        return false;
    }

    HistoryQueryResult history;
    if (!repository.loadRecentForUser("Alice", std::nullopt, 50, history))
    {
        std::cerr << "无法查询幂等测试历史记录\n";
        return false;
    }
    if (history.messages.size() != 1 || history.messages.front().message_id != first_outcome.message_id ||
        history.messages.front().content != first_message.content)
    {
        std::cerr << "重复请求改变了数据库中的原始记录\n";
        return false;
    }

    return true;
}

// 为单个合同场景创建独立夹具，执行后先关闭 Repository，再清理后端资源。
bool runContractTest(const char *name, const RepositoryFactory &factory, const ContractTest contract_test)
{
    RepositoryFixture fixture = factory();
    bool passed = false;
    if (!fixture.repository)
    {
        std::cerr << "合同测试 Factory 未返回可用的 Repository\n";
    }
    else
    {
        passed = contract_test(*fixture.repository);
    }

    fixture.repository.reset();
    if (fixture.cleanup && !fixture.cleanup())
    {
        passed = false;
    }

    if (passed)
    {
        std::cout << "PASS [contract]: " << name << '\n';
    }
    else
    {
        std::cerr << "FAIL [contract]: " << name << '\n';
    }
    return passed;
}

} // namespace

// 测试类型：公共合同测试入口。
// 测试内容：确保每个业务场景都通过 Factory 获得独立 Repository，并汇总全部合同结果。
bool runMessageRepositoryContractTests(const RepositoryFactory &factory)
{
    const bool history_passed =
        runContractTest("history query and composite cursor", factory, testHistoryQueryContract);
    const bool idempotency_passed = runContractTest("store idempotency", factory, testStoreIdempotencyContract);
    return history_passed && idempotency_passed;
}

} // namespace repository::test
