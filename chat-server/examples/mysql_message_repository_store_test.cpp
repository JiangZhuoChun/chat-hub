#include "repository/mysql_connection.h"
#include "repository/mysql_message_repository.h"

#include <string>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mysql.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{

class TwoWorkerGate
{
public:
    void arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        ++m_arrived;
        m_ready.notify_one();

        m_release.wait(lock,[this] {
           return m_released;
        });
    }

    void releaseAfterBothArrived()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_ready.wait(lock,[this] {
           return m_arrived == 2;
        });

        m_released = true;
        lock.unlock();
        m_release.notify_all();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::condition_variable m_release;
    int m_arrived{0};
    bool m_released{false};
};
constexpr int kSkipMissingMySqlConfiguration = 77;
constexpr char kExpectedDatabase[] = "chathub_schema_test";

// 两组固定夹具分别服务于“完全重试”和“幂等冲突”，避免两个测试互相依赖。
struct MessageFixture
{
    const char* message_id;
    const char* sender;
    const char* recipient;
    const char* client_local_id;
    const char* content;
    const char* client_send_at;
    std::int64_t server_received_at_ms;
};
//保存每个工作线程的连接结果和业务结果
struct ConcurrentWorkerResult
{
    repository::MySqlConnectionStatus open_status{repository::MySqlConnectionStatus::ConnectionFailed};
    repository::StoreOutcome outcome{repository::StoreResult::DatabaseError};
};

constexpr MessageFixture kDuplicateFixture{
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    "alice",
    "bob",
    "duplicate-001",
    "same content",
    "2026-08-23T10:00:00Z",
    1000,
};

constexpr MessageFixture kConflictFixture{
    "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
    "alice",
    "bob",
    "conflict-001",
    "original content",
    "2026-08-23T10:01:00Z",
    2000,
};

// 首次写入使用独立的幂等键，确保它不会依赖或干扰前两个“已有旧记录”的场景。
constexpr MessageFixture kStoredFixture{
    "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE",
    "carol",
    "dave",
    "stored-001",
    "new content",
    "2026-08-23T10:02:00Z",
    3000,
};

// 两个并发请求共享同一幂等键和业务正文，但候选 ID 与服务端接收时间不同。
// 胜者不可预测；败者必须返回胜者已经持久化的完整记录。
constexpr MessageFixture kConcurrentFixture{
    "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    "erin",
    "frank",
    "concurrent-001",
    "race content",
    "2026-08-24T10:00:00Z",
    4000,
};

// 占位行只用于占用主键；它的 sender/local_id 与待恢复请求不同，确保失败来自 message_id 冲突。
constexpr MessageFixture kPrimaryKeyBlockerFixture{
    "11111111111111111111111111111111",
    "blocker",
    "discard",
    "blocker-001",
    "primary key blocker",
    "2026-08-24T10:01:00Z",
    5000,
};

// 此请求的候选 message_id 故意与占位行相同；移除占位行后，同一请求应能安全重试成功。
constexpr MessageFixture kRecoverableFailureFixture{
    "11111111111111111111111111111111",
    "gina",
    "harry",
    "recoverable-001",
    "recoverable content",
    "2026-08-24T10:02:00Z",
    6000,
};

// 独立连接写入的消息不共享失败请求的主键或幂等键，用于验证故障隔离。
constexpr MessageFixture kIndependentConnectionFixture{
    "22222222222222222222222222222222",
    "irene",
    "john",
    "independent-001",
    "independent connection content",
    "2026-08-24T10:03:00Z",
    7000,
};

constexpr char kDuplicateCandidateMessageId[] =
    "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
constexpr char kConflictCandidateMessageId[] =
    "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD";
constexpr char kConcurrentSecondCandidateMessageId[] =
    "99999999999999999999999999999999";

bool resetSchema(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlConnection connection;
    if (connection.open(config) != repository::MySqlConnectionStatus::Opened)
    {
        return false;
    }

    MYSQL* const handle = connection.nativeHandle();
    const bool messages_removed =
        handle != nullptr && mysql_query(handle, "DROP TABLE IF EXISTS messages") == 0;
    const bool migrations_removed =
        messages_removed && mysql_query(handle, "DROP TABLE IF EXISTS schema_migrations") == 0;

    connection.close();
    return migrations_removed;
}

bool openAndInitialize(repository::MySqlMessageRepository& repository,
                       const repository::MySqlConnectionConfig& config)
{
    return repository.open(config) == repository::MySqlConnectionStatus::Opened &&
           repository.initializeSchema();
}

// 测试夹具全部是本文件内的固定文本，因此这里的 SQL 不含任何外部输入。
bool seedExistingMessage(const repository::MySqlConnectionConfig& config,
                          const MessageFixture& fixture)
{
    repository::MySqlConnection connection;
    if (connection.open(config) != repository::MySqlConnectionStatus::Opened)
    {
        return false;
    }

    MYSQL* const handle = connection.nativeHandle();
    if (handle == nullptr)
    {
        return false;
    }

    const std::string statement =
        std::string("INSERT INTO messages ") +
        "(message_id, sender, recipient, client_local_id, content, client_send_at, "
        "server_received_at_ms) VALUES ('" +
        fixture.message_id + "', '" + fixture.sender + "', '" + fixture.recipient +
        "', '" + fixture.client_local_id + "', '" + fixture.content + "', '" +
        fixture.client_send_at + "', " +
        std::to_string(fixture.server_received_at_ms) + ")";

    return mysql_query(handle, statement.c_str()) == 0;
}

// 测试夹具全部使用固定 ID；删除前要求恰好影响一行，避免把“故障仍存在”误判为恢复成功。
bool removeMessageById(const repository::MySqlConnectionConfig& config,
                       const char* const message_id)
{
    repository::MySqlConnection connection;
    if (connection.open(config) != repository::MySqlConnectionStatus::Opened ||
        message_id == nullptr || *message_id == '\0')
    {
        return false;
    }

    MYSQL* const handle = connection.nativeHandle();
    const std::string statement =
        "DELETE FROM messages WHERE message_id = '" + std::string(message_id) + "'";
    const bool removed =
        handle != nullptr && mysql_query(handle, statement.c_str()) == 0 &&
        mysql_affected_rows(handle) == 1;
    connection.close();
    return removed;
}

bool readPersistedContent(const repository::MySqlConnectionConfig& config,
                          const MessageFixture& fixture,
                          std::string& out_content)
{
    out_content.clear();

    repository::MySqlConnection connection;
    if (connection.open(config) != repository::MySqlConnectionStatus::Opened)
    {
        return false;
    }

    MYSQL* const handle = connection.nativeHandle();
    if (handle == nullptr)
    {
        return false;
    }

    const std::string query =
        "SELECT content FROM messages WHERE sender = '" +
        std::string(fixture.sender) + "' AND client_local_id = '" +
        fixture.client_local_id + "'";
    if (mysql_query(handle, query.c_str()) != 0)
    {
        return false;
    }

    using ResultOwner = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;
    ResultOwner result{mysql_store_result(handle), &mysql_free_result};
    if (!result || mysql_num_rows(result.get()) != 1)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (row == nullptr || row[0] == nullptr)
    {
        return false;
    }

    out_content = row[0];
    return true;
}

repository::NewMessage makeRequest(const MessageFixture& fixture,
                                   const char* candidate_message_id,
                                   const std::string& content,
                                   const std::int64_t server_received_at_ms)
{
    return {
        fixture.sender,
        fixture.recipient,
        content,
        fixture.client_send_at,
        fixture.client_local_id,
        server_received_at_ms,
        candidate_message_id,
    };
}

void runConcurrentStore(const repository::MySqlConnectionConfig& config,
                        const repository::NewMessage& request,TwoWorkerGate& gate,
                        ConcurrentWorkerResult& result)
{
    repository::MySqlMessageRepository repository;

    result.open_status = repository.open(config);
    gate.arriveAndWait();

    if (result.open_status != repository::MySqlConnectionStatus::Opened)
    {
        return;
    }

    result.outcome = repository.storeMessage(request);
    repository.close();
}

// Integration behavior: an exact retry returns the original persisted record, not this retry's candidate ID or time.
bool testDuplicateSameReturnsOriginalRecord(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    if (!openAndInitialize(repository, config) ||
        !seedExistingMessage(config, kDuplicateFixture))
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "duplicate fixture setup failed\n";
        return false;
    }

    const repository::NewMessage retry = makeRequest(
        kDuplicateFixture,
        kDuplicateCandidateMessageId,
        kDuplicateFixture.content,
        9999);
    const repository::StoreOutcome outcome = repository.storeMessage(retry);

    const bool passed =
        outcome.result == repository::StoreResult::DuplicateSame &&
        outcome.message_id == kDuplicateFixture.message_id &&
        outcome.sender == kDuplicateFixture.sender &&
        outcome.recipient == kDuplicateFixture.recipient &&
        outcome.content == kDuplicateFixture.content &&
        outcome.client_send_at == kDuplicateFixture.client_send_at &&
        outcome.server_received_at_ms == kDuplicateFixture.server_received_at_ms;
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "exact retry did not return the original persisted record\n";
    }
    return passed;
}

// Integration behavior: a reused local ID with changed business fields is rejected and leaves the old row unchanged.
bool testIdempotencyConflictDoesNotModifyExistingRecord(
    const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    if (!openAndInitialize(repository, config) ||
        !seedExistingMessage(config, kConflictFixture))
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "conflict fixture setup failed\n";
        return false;
    }

    const repository::NewMessage conflicting_request = makeRequest(
        kConflictFixture,
        kConflictCandidateMessageId,
        "changed content",
        9999);
    const repository::StoreOutcome outcome = repository.storeMessage(conflicting_request);

    std::string persisted_content;
    const bool passed =
        outcome.result == repository::StoreResult::IdempotencyConflict &&
        readPersistedContent(config, kConflictFixture, persisted_content) &&
        persisted_content == kConflictFixture.content;
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "conflicting retry changed or failed to preserve the original row\n";
    }
    return passed;
}

// Integration behavior: the first request for an unused idempotency key is committed,
// returns its complete business result, and leaves a queryable database row behind.
bool testFirstMessageIsStored(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    if (!openAndInitialize(repository, config))
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "stored fixture setup failed\n";
        return false;
    }

    const repository::NewMessage request = makeRequest(
        kStoredFixture,
        kStoredFixture.message_id,
        kStoredFixture.content,
        kStoredFixture.server_received_at_ms);
    const repository::StoreOutcome outcome = repository.storeMessage(request);

    std::string persisted_content;
    const bool passed =
        outcome.result == repository::StoreResult::Stored &&
        outcome.message_id == kStoredFixture.message_id &&
        outcome.sender == kStoredFixture.sender &&
        outcome.recipient == kStoredFixture.recipient &&
        outcome.content == kStoredFixture.content &&
        outcome.client_send_at == kStoredFixture.client_send_at &&
        outcome.server_received_at_ms == kStoredFixture.server_received_at_ms &&
        readPersistedContent(config, kStoredFixture, persisted_content) &&
        persisted_content == kStoredFixture.content;
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "first request was not stored with its complete result\n";
    }
    return passed;
}

// Integration behavior: two independently connected concurrent callers using one
// idempotency key leave exactly one row and receive one Stored plus one DuplicateSame.
bool testConcurrentSameRequestHasOneStoredAndOneDuplicateSame(
    const repository::MySqlConnectionConfig& config)
{
    // Schema 初始化必须在线程启动前完成；本测试只竞争消息写入，不竞争迁移。
    repository::MySqlMessageRepository schema_repository;
    if (!openAndInitialize(schema_repository, config))
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "concurrent fixture schema setup failed\n";
        return false;
    }
    schema_repository.close();

    const repository::NewMessage first_request = makeRequest(
        kConcurrentFixture,
        kConcurrentFixture.message_id,
        kConcurrentFixture.content,
        kConcurrentFixture.server_received_at_ms);
    const repository::NewMessage second_request = makeRequest(
        kConcurrentFixture,
        kConcurrentSecondCandidateMessageId,
        kConcurrentFixture.content,
        9999);

    TwoWorkerGate gate;
    ConcurrentWorkerResult first_result;
    ConcurrentWorkerResult second_result;

    // 每个工作线程在自己的栈上创建 Repository，因此绝不共享 MYSQL* 或事务状态。
    std::thread first_worker([&] {
        runConcurrentStore(config, first_request, gate, first_result);
    });
    std::thread second_worker([&] {
        runConcurrentStore(config, second_request, gate, second_result);
    });

    // 只有两个连接都到达起跑线后才放行，不使用不稳定的 sleep。
    gate.releaseAfterBothArrived();
    first_worker.join();
    second_worker.join();

    const bool connections_opened =
        first_result.open_status == repository::MySqlConnectionStatus::Opened &&
        second_result.open_status == repository::MySqlConnectionStatus::Opened;
    const bool first_stored =
        first_result.outcome.result == repository::StoreResult::Stored;
    const bool second_stored =
        second_result.outcome.result == repository::StoreResult::Stored;

    // 调度顺序不可预测，所以只断言“恰好一个胜者”，不能断言第一个线程一定获胜。
    if (!connections_opened || first_stored == second_stored)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "concurrent callers did not produce exactly one stored result\n";
        return false;
    }

    const repository::StoreOutcome& stored =
        first_stored ? first_result.outcome : second_result.outcome;
    const repository::NewMessage& stored_request =
        first_stored ? first_request : second_request;
    const repository::StoreOutcome& duplicate =
        first_stored ? second_result.outcome : first_result.outcome;

    std::string persisted_content;
    const bool passed =
        stored.message_id == stored_request.message_id &&
        stored.sender == stored_request.sender &&
        stored.recipient == stored_request.recipient &&
        stored.content == stored_request.content &&
        stored.client_send_at == stored_request.client_send_at &&
        stored.server_received_at_ms == stored_request.server_received_at_ms &&
        duplicate.result == repository::StoreResult::DuplicateSame &&
        duplicate.message_id == stored.message_id &&
        duplicate.sender == stored.sender &&
        duplicate.recipient == stored.recipient &&
        duplicate.content == stored.content &&
        duplicate.client_send_at == stored.client_send_at &&
        duplicate.server_received_at_ms == stored.server_received_at_ms &&
        // 该辅助函数要求 SELECT 恰好返回一行，因而同时证明数据库中没有重复行。
        readPersistedContent(config, kConcurrentFixture, persisted_content) &&
        persisted_content == stored.content;
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "concurrent idempotency result or persisted row was inconsistent\n";
    }
    return passed;
}

// Integration behavior: a failed INSERT rolls back cleanly, does not break an
// independent connection, and the original request can be retried after the fault is removed.
bool testPrimaryKeyFailureIsIsolatedAndRecoverable(
    const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository failing_repository;
    if (!openAndInitialize(failing_repository, config) ||
        !seedExistingMessage(config, kPrimaryKeyBlockerFixture))
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "recoverable-failure fixture setup failed\n";
        return false;
    }

    const repository::NewMessage recoverable_request = makeRequest(
        kRecoverableFailureFixture,
        kRecoverableFailureFixture.message_id,
        kRecoverableFailureFixture.content,
        kRecoverableFailureFixture.server_received_at_ms);
    const repository::StoreOutcome failed_outcome =
        failing_repository.storeMessage(recoverable_request);

    // 另一个 Repository 使用独立 MYSQL*；它应不受前一个请求回滚的影响。
    repository::MySqlMessageRepository independent_repository;
    const repository::NewMessage independent_request = makeRequest(
        kIndependentConnectionFixture,
        kIndependentConnectionFixture.message_id,
        kIndependentConnectionFixture.content,
        kIndependentConnectionFixture.server_received_at_ms);
    const bool independent_connection_is_usable =
        independent_repository.open(config) == repository::MySqlConnectionStatus::Opened &&
        independent_repository.storeMessage(independent_request).result == repository::StoreResult::Stored;
    independent_repository.close();

    // 只移除测试制造的主键冲突，再对完全相同的业务请求重试。
    const bool blocker_removed =
        removeMessageById(config, kPrimaryKeyBlockerFixture.message_id);
    const repository::StoreOutcome recovered_outcome = blocker_removed
        ? failing_repository.storeMessage(recoverable_request)
        : repository::StoreOutcome{repository::StoreResult::DatabaseError};

    std::string persisted_content;
    const bool recovered_row_is_unique =
        readPersistedContent(config, kRecoverableFailureFixture, persisted_content) &&
        persisted_content == kRecoverableFailureFixture.content;
    const bool passed =
        failed_outcome.result == repository::StoreResult::DatabaseError &&
        failing_repository.isOpen() &&
        independent_connection_is_usable &&
        blocker_removed &&
        recovered_outcome.result == repository::StoreResult::Stored &&
        recovered_outcome.message_id == recoverable_request.message_id &&
        recovered_row_is_unique;
    failing_repository.close();

    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "primary-key failure was not isolated or recoverable\n";
    }
    return passed;
}
} // namespace

int main()
{
    const char* const username = std::getenv("CHATHUB_MYSQL_REPOSITORY_TEST_USERNAME");
    const char* const password = std::getenv("CHATHUB_MYSQL_REPOSITORY_TEST_PASSWORD");
    const char* const database = std::getenv("CHATHUB_MYSQL_REPOSITORY_TEST_DATABASE");
    if (username == nullptr || password == nullptr || database == nullptr ||
        *username == '\0' || *password == '\0' || *database == '\0')
    {
        std::cerr << "SKIP [mysql-message-repository-store]: "
                     "missing MySQL repository test configuration\n";
        return kSkipMissingMySqlConfiguration;
    }

    if (std::string(database) != kExpectedDatabase)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "unexpected repository test database name\n";
        return EXIT_FAILURE;
    }

    repository::MySqlConnectionConfig config;
    config.username = username;
    config.password = password;
    config.database = database;

    const bool initial_cleanup_passed = resetSchema(config);
    if (!initial_cleanup_passed)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "initial schema reset failed\n";
    }

    // 不用 && 串联，保证即使前一个断言失败，另一个行为场景仍会执行并给出证据。
    const bool duplicate_passed = initial_cleanup_passed &&
                                  testDuplicateSameReturnsOriginalRecord(config);
    const bool conflict_passed = initial_cleanup_passed &&
                                 testIdempotencyConflictDoesNotModifyExistingRecord(config);
    const bool stored_passed = initial_cleanup_passed &&
                               testFirstMessageIsStored(config);
    const bool concurrent_passed = initial_cleanup_passed &&
                                    testConcurrentSameRequestHasOneStoredAndOneDuplicateSame(config);
    const bool recoverable_failure_passed = initial_cleanup_passed &&
                                            testPrimaryKeyFailureIsIsolatedAndRecoverable(config);
    const bool final_cleanup_passed = resetSchema(config);
    if (!final_cleanup_passed)
    {
        std::cerr << "FAIL [mysql-message-repository-store]: "
                     "final schema reset failed\n";
    }

    return duplicate_passed && conflict_passed && stored_passed && concurrent_passed &&
                   recoverable_failure_passed && final_cleanup_passed
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
