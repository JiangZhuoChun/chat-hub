#include "repository/mysql_connection.h"
#include "repository/mysql_message_repository.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mysql.h>
#include <optional>
#include <string>

namespace
{
constexpr int kSkipMissingMySqlConfiguration = 77;
constexpr char kExpectedDatabase[] = "chathub_schema_test";

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

// Alice 的三条相关消息中，后两条时间相同，必须由 message_id 决定稳定顺序。
constexpr MessageFixture kAliceOlder{
    "00000000000000000000000000000001",
    "Alice",
    "Bob",
    "alice-local-001",
    "message at 1000",
    "client-time-1000",
    1000,
};

constexpr MessageFixture kAliceSameTimeLowerId{
    "00000000000000000000000000000002",
    "Bob",
    "Alice",
    "bob-local-001",
    "message at 2000 lower id",
    "client-time-2000-b",
    2000,
};

constexpr MessageFixture kAliceSameTimeHigherId{
    "00000000000000000000000000000003",
    "Alice",
    "Bob",
    "alice-local-002",
    "message at 2000 higher id",
    "client-time-2000-c",
    2000,
};

// 时间更晚但与 Alice 无关，验证 WHERE sender = ? OR recipient = ? 的用户隔离。
constexpr MessageFixture kUnrelatedMessage{
    "00000000000000000000000000000004",
    "Carol",
    "Dave",
    "carol-local-001",
    "unrelated message",
    "client-time-3000",
    3000,
};

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

bool resetAndOpen(repository::MySqlMessageRepository& repository,
                  const repository::MySqlConnectionConfig& config)
{
    return resetSchema(config) &&
           repository.open(config) == repository::MySqlConnectionStatus::Opened &&
           repository.initializeSchema();
}

bool storeFixture(repository::MySqlMessageRepository& repository,
                  const MessageFixture& fixture)
{
    const repository::NewMessage message{
        fixture.sender,
        fixture.recipient,
        fixture.content,
        fixture.client_send_at,
        fixture.client_local_id,
        fixture.server_received_at_ms,
        fixture.message_id,
    };
    return repository.storeMessage(message).result == repository::StoreResult::Stored;
}

bool storeAliceHistoryFixtures(repository::MySqlMessageRepository& repository)
{
    return storeFixture(repository, kAliceOlder) &&
           storeFixture(repository, kAliceSameTimeLowerId) &&
           storeFixture(repository, kAliceSameTimeHigherId) &&
           storeFixture(repository, kUnrelatedMessage);
}

// Integration behavior: an empty first page is a successful query, not a database error.
bool testEmptyFirstPage(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    if (!resetAndOpen(repository, config))
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "empty-history fixture setup failed\n";
        return false;
    }

    repository::HistoryQueryResult result;
    const bool passed =
        repository.loadRecentForUser("nobody", std::nullopt, 50, result) &&
        result.messages.empty() &&
        !result.has_more &&
        !result.next_cursor.has_value();
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "empty first page did not return an empty successful result\n";
    }
    return passed;
}

// Integration behavior: the first page filters unrelated rows, returns old-to-new,
// and uses the oldest displayed row as the next cursor when more history exists.
bool testFirstPageFiltersOrdersAndBuildsCursor(
    const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    if (!resetAndOpen(repository, config) || !storeAliceHistoryFixtures(repository))
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "first-page fixture setup failed\n";
        return false;
    }

    repository::HistoryQueryResult result;
    if (!repository.loadRecentForUser("Alice", std::nullopt, 2, result))
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "first page query failed\n";
        return false;
    }

    const bool only_alice_messages =
        result.messages.size() == 2 &&
        (result.messages[0].sender == "Alice" || result.messages[0].recipient == "Alice") &&
        (result.messages[1].sender == "Alice" || result.messages[1].recipient == "Alice");
    const bool passed =
        result.messages.size() == 2 &&
        result.messages[0].message_id == kAliceSameTimeLowerId.message_id &&
        result.messages[1].message_id == kAliceSameTimeHigherId.message_id &&
        only_alice_messages &&
        result.messages[1].sender == "Alice" &&
        result.has_more &&
        result.next_cursor.has_value() &&
        result.next_cursor->server_received_at_ms == 2000 &&
        result.next_cursor->message_id == kAliceSameTimeLowerId.message_id;
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "first page did not preserve filter, order, or cursor contract\n";
    }
    return passed;
}

// Integration behavior: a non-positive requested limit is normalized to one message.
bool testFirstPageClampsZeroLimit(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    if (!resetAndOpen(repository, config) || !storeAliceHistoryFixtures(repository))
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "zero-limit fixture setup failed\n";
        return false;
    }

    repository::HistoryQueryResult result;
    const bool passed =
        repository.loadRecentForUser("Alice", std::nullopt, 0, result) &&
        result.messages.size() == 1 &&
        result.messages.front().message_id == kAliceSameTimeHigherId.message_id &&
        result.has_more &&
        result.next_cursor.has_value() &&
        result.next_cursor->server_received_at_ms == 2000 &&
        result.next_cursor->message_id == kAliceSameTimeHigherId.message_id;
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "zero limit was not normalized to one message\n";
    }
    return passed;
}

// Integration behavior: the second page begins strictly before the first page's
// oldest row, so the two pages contain every Alice message exactly once.
bool testCursorPageHasNoDuplicateAndNoGap(
    const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    if (!resetAndOpen(repository, config) || !storeAliceHistoryFixtures(repository))
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "cursor-page fixture setup failed\n";
        return false;
    }

    repository::HistoryQueryResult first_page;
    if (!repository.loadRecentForUser("Alice", std::nullopt, 2, first_page) ||
        !first_page.next_cursor.has_value())
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "first page did not provide a usable cursor\n";
        return false;
    }

    repository::HistoryQueryResult second_page;
    if (!repository.loadRecentForUser("Alice", first_page.next_cursor, 2, second_page))
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "second page query failed\n";
        return false;
    }

    const bool first_page_is_expected =
        first_page.messages.size() == 2 &&
        first_page.messages[0].message_id == kAliceSameTimeLowerId.message_id &&
        first_page.messages[1].message_id == kAliceSameTimeHigherId.message_id &&
        first_page.has_more;
    const bool second_page_is_expected =
        second_page.messages.size() == 1 &&
        second_page.messages[0].message_id == kAliceOlder.message_id &&
        !second_page.has_more &&
        !second_page.next_cursor.has_value();
    const bool no_duplicate =
        second_page.messages.size() == 1 &&
        second_page.messages[0].message_id != first_page.messages[0].message_id &&
        second_page.messages[0].message_id != first_page.messages[1].message_id;
    const bool passed = first_page_is_expected && second_page_is_expected && no_duplicate;
    if (!passed)
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "cursor pages contained a duplicate, omission, or wrong order\n";
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
        std::cerr << "SKIP [mysql-message-repository-history]: "
                     "missing MySQL repository test configuration\n";
        return kSkipMissingMySqlConfiguration;
    }

    if (std::string(database) != kExpectedDatabase)
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "unexpected repository test database name\n";
        return EXIT_FAILURE;
    }

    repository::MySqlConnectionConfig config;
    config.username = username;
    config.password = password;
    config.database = database;

    const bool empty_page_passed = testEmptyFirstPage(config);
    const bool first_page_passed = testFirstPageFiltersOrdersAndBuildsCursor(config);
    const bool zero_limit_passed = testFirstPageClampsZeroLimit(config);
    const bool cursor_page_passed = testCursorPageHasNoDuplicateAndNoGap(config);
    const bool final_cleanup_passed = resetSchema(config);
    if (!final_cleanup_passed)
    {
        std::cerr << "FAIL [mysql-message-repository-history]: "
                     "final schema reset failed\n";
    }

    return empty_page_passed && first_page_passed && zero_limit_passed && cursor_page_passed &&
                   final_cleanup_passed
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
