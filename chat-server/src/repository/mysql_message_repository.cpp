#include "repository/mysql_message_repository.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mysql.h>
#include <string>

namespace
{
/*---------------------------------------------------------------
 *PRIMARY KEY (message_id)：全局消息身份唯一
 *UNIQUE KEY (sender, client_local_id)：数据库层面的幂等底线
 *VARCHAR(20)：对应协议用户名上限
 *utf8mb4_bin：按精确字符值比较 local_id，不把大小写不同的 ID 混为同一个
 *ENGINE = InnoDB：MySQL 的事务型存储引擎；后续消息写入的事务与唯一约束依赖它
 *两个 KEY：分别加速“我是发送者”和“我是接收者”的历史倒序查询
 -----------------------------------------------------------------*/
constexpr char kCreateMessagesTableV1[] = R"(
    CREATE TABLE IF NOT EXISTS messages(
        message_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        sender VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        recipient VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        client_local_id VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        content TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        client_send_at VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        server_received_at_ms BIGINT NOT NULL,

        PRIMARY KEY (message_id),
        UNIQUE KEY uq_message_sender_local_id (sender,client_local_id),
        KEY idx_messages_sender_order(
            sender,server_received_at_ms DESC,message_id DESC
        ),
        KEY idx_messages_recipient_order(
            recipient,server_received_at_ms DESC,message_id DESC
        )
    )ENGINE = InnoDB
)";

constexpr char kCreateSchemaMigrationsTable[] = R"(
    CREATE TABLE IF NOT EXISTS schema_migrations(
        version INT NOT NULL PRIMARY KEY)
     ENGINE = InnoDB
)";
} // namespace
namespace repository
{

namespace
{
// 查询不是二元结果：只有确实没有行时才是 NotFound；任何 MySQL API 失败都必须保留为 DatabaseError。
enum class ExistingMessageLookupResult
{
    Found,
    NotFound,
    DatabaseError
};

// 这是幂等判定所需的旧记录快照；不包含 client_local_id，因为查询条件已确定它相同。
struct ExistingMessageRecord
{
    std::string message_id;
    std::string recipient;
    std::string content;
    std::string client_send_at;
    std::int64_t server_received_at_ms{};
};

constexpr char kStartTransactionSql[] = "START TRANSACTION";

// 普通重查不持有行锁，供插入失败、回滚完成后的“确认真实结果”路径使用。
constexpr char kSelectExistingMessage[] = R"(
    SELECT message_id,recipient,content,client_send_at,server_received_at_ms
    FROM messages
    WHERE sender = ? AND client_local_id = ?
)";

// 首次查询在事务内使用 FOR UPDATE：若已有该幂等键，先锁住旧记录再决定返回何种业务结果。
constexpr char kSelectExistingMessageForUpdate[] = R"(
    SELECT message_id,recipient,content,client_send_at,server_received_at_ms
    FROM messages
    WHERE sender = ? AND client_local_id = ?
    FOR UPDATE
)";

constexpr char kInsertMessageSql[] = R"(
    INSERT INTO messages
    (message_id,sender,recipient,client_local_id,content,client_send_at,server_received_at_ms)
    VALUES(?,?,?,?,?,?,?)
)";

constexpr char kSelectRecentMessagesFirstPageSql[] = R"(
    SELECT message_id,sender,recipient,client_local_id,content,client_send_at,
           server_received_at_ms
    FROM messages
    WHERE sender = ? OR recipient = ?
    ORDER BY server_received_at_ms DESC,message_id DESC
    LIMIT ?
)";

constexpr char kSelectRecentMessagesBeforeCursorSql[] = R"(
    SELECT message_id,sender,recipient,client_local_id,content,client_send_at,
           server_received_at_ms
    FROM messages
    WHERE (sender = ? OR recipient = ?)
      AND (
            server_received_at_ms < ?
            OR (
                server_received_at_ms = ?
                AND message_id < ?
            )
          )
    ORDER BY server_received_at_ms DESC,message_id DESC
    LIMIT ?
)";

// 按 (sender, client_local_id) 查找已有逻辑请求。
// lock_for_update 只允许在已开始的事务中传 true；查询成功时把旧记录填入 out_record。
// 函数借用 connection，不拥有它，因此绝不能在这里调用 mysql_close()。
ExistingMessageLookupResult findExistingMessage(MYSQL *connection, const NewMessage &message,
                                                const bool lock_for_update, ExistingMessageRecord &out_record)

{
    // 先清空输出，并检查前提
    out_record = {};
    if (connection == nullptr || message.sender.empty() || message.client_local_id.empty())
    {
        return ExistingMessageLookupResult::DatabaseError;
    }

    using StatementOwner = std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)>;
    // mysql_stmt_init(connection) 创建工作单。
    const StatementOwner statement{mysql_stmt_init(connection), &mysql_stmt_close};
    if (!statement)
    {
        return ExistingMessageLookupResult::DatabaseError;
    }

    if (const char *sql = lock_for_update ? kSelectExistingMessageForUpdate : kSelectExistingMessage;
        // 准备 SQL，不执行
        mysql_stmt_prepare(statement.get(), sql, static_cast<unsigned long>(std::strlen(sql))) != 0)
    {
        return ExistingMessageLookupResult::DatabaseError;
    }

    // MySQL C API 绑定字符串时，不只需要内存地址，还需要实际字节长度；两个数组元素依次对应 SQL 中两个 ?。
    auto sender_length = static_cast<unsigned long>(message.sender.size());
    auto local_id_length = static_cast<unsigned long>(message.client_local_id.size());
    // MYSQL_BIND：给两个 ? 填值
    std::array<MYSQL_BIND, 2> parameters{};

    parameters[0].buffer_type = MYSQL_TYPE_STRING;                    // 第一个 ? 是字符串
    parameters[0].buffer = const_cast<char *>(message.sender.data()); // 字符串内容在内存中的地址
    parameters[0].buffer_length = sender_length;                      // 这段内存最多有多大
    parameters[0].length = &sender_length;                            // 实际参数长度是多少

    parameters[1].buffer_type = MYSQL_TYPE_STRING;
    parameters[1].buffer = const_cast<char *>(message.client_local_id.data());
    parameters[1].buffer_length = local_id_length;
    parameters[1].length = &local_id_length;

    // 第一步： mysql_stmt_bind_param(...)把两个参数正式交给语句。
    // 第二步： mysql_stmt_execute(...)真正执行 SQL。
    // 第三步： mysql_stmt_store_result(...) 把 SELECT 的完整结果集缓存在客户端
    if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0 || mysql_stmt_execute(statement.get()) != 0 ||
        mysql_stmt_store_result(statement.get()) != 0)
    {
        return ExistingMessageLookupResult::DatabaseError;
    }
    // MySQL 不会直接构造 C++ string：调用者必须为 SELECT 的每一列预留接收缓冲区。
    std::array<char, 32> message_id_buffer{};
    std::array<char, 20> recipient_buffer{};
    std::array<char, 1024> content_buffer{};
    std::array<char, 64> client_send_at_buffer{};
    std::int64_t server_received_at_ms{};

    // lengths 是每列实际写入的字节数；is_null 和 errors 让我们识别 NULL 或缓冲区截断，避免把不完整数据当成功。
    std::array<unsigned long, 5> lengths{};
    std::array<my_bool, 5> is_null{};
    std::array<my_bool, 5> errors{};
    std::array<MYSQL_BIND, 5> results{};

    results[0].buffer_type = MYSQL_TYPE_STRING;
    results[0].buffer = message_id_buffer.data();
    results[0].buffer_length = static_cast<unsigned long>(message_id_buffer.size());
    results[0].length = &lengths[0];
    results[0].is_null = &is_null[0];
    results[0].error = &errors[0];

    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = recipient_buffer.data();
    results[1].buffer_length = static_cast<unsigned long>(recipient_buffer.size());
    results[1].length = &lengths[1];
    results[1].is_null = &is_null[1];
    results[1].error = &errors[1];

    results[2].buffer_type = MYSQL_TYPE_STRING;
    results[2].buffer = content_buffer.data();
    results[2].buffer_length = static_cast<unsigned long>(content_buffer.size());
    results[2].length = &lengths[2];
    results[2].is_null = &is_null[2];
    results[2].error = &errors[2];

    results[3].buffer_type = MYSQL_TYPE_STRING;
    results[3].buffer = client_send_at_buffer.data();
    results[3].buffer_length = static_cast<unsigned long>(client_send_at_buffer.size());
    results[3].length = &lengths[3];
    results[3].is_null = &is_null[3];
    results[3].error = &errors[3];

    results[4].buffer_type = MYSQL_TYPE_LONGLONG;
    results[4].buffer = &server_received_at_ms;
    results[4].is_null = &is_null[4];
    results[4].error = &errors[4];

    // SELECT 返回的五列按 SQL 的列顺序分别写入 results[0] 到 results[4]。
    if (mysql_stmt_bind_result(statement.get(), results.data()) != 0)
    {
        return ExistingMessageLookupResult::DatabaseError;
    }

    // MYSQL_NO_DATA 是“没有旧记录”的正常结果；其他非零值表示读取失败或数据截断。
    const int fetch_result = mysql_stmt_fetch(statement.get());
    if (fetch_result == MYSQL_NO_DATA)
    {
        return ExistingMessageLookupResult::NotFound;
    }
    if (fetch_result != 0)
    {
        return ExistingMessageLookupResult::DatabaseError;
    }

    for (const my_bool error : errors)
    {
        if (error)
        {
            return ExistingMessageLookupResult::DatabaseError;
        }
    }
    for (const my_bool null_value : is_null)
    {
        if (null_value)
        {
            return ExistingMessageLookupResult::DatabaseError;
        }
    }

    // 用数据库报告的精确长度构造 string，不依赖缓冲区末尾是否存在 '\0'。
    out_record.message_id = std::string(message_id_buffer.data(), lengths[0]);
    out_record.recipient = std::string(recipient_buffer.data(), lengths[1]);
    out_record.content = std::string(content_buffer.data(), lengths[2]);
    out_record.client_send_at = std::string(client_send_at_buffer.data(), lengths[3]);
    out_record.server_received_at_ms = server_received_at_ms;

    return ExistingMessageLookupResult::Found;
}

bool insertCandidateMessage(MYSQL* connection, const NewMessage &message)
{
    if (connection == nullptr || message.message_id.empty())
    {
        return false;
    }

    using StatementOwner = std::unique_ptr<MYSQL_STMT,decltype(&mysql_stmt_close)>;
    const StatementOwner statement{mysql_stmt_init(connection), &mysql_stmt_close};
    if (!statement || mysql_stmt_prepare(
        statement.get(),kInsertMessageSql,static_cast<unsigned long>(std::strlen(kInsertMessageSql))) != 0)
    {
        return false;
    }

    auto message_id_length =static_cast<unsigned long>(message.message_id.size());
    auto sender_length =static_cast<unsigned long>(message.sender.size());
    auto recipient_length =static_cast<unsigned long>(message.recipient.size());
    auto local_id_length =static_cast<unsigned long>(message.client_local_id.size());
    auto content_length =static_cast<unsigned long>(message.content.size());
    auto client_send_at_length =static_cast<unsigned long>(message.client_send_at.size());

    // 数值也必须在 execute() 完成前持续有效，因此复制到局部变量。
    std::int64_t server_received_at_ms = message.server_received_at_ms;

    std::array<MYSQL_BIND,7> parameters{};
    parameters[0].buffer_type = MYSQL_TYPE_STRING;
    parameters[0].buffer = const_cast<char*>(message.message_id.data());
    parameters[0].buffer_length = message_id_length;
    parameters[0].length = &message_id_length;

    parameters[1].buffer_type = MYSQL_TYPE_STRING;
    parameters[1].buffer = const_cast<char*>(message.sender.data());
    parameters[1].buffer_length = sender_length;
    parameters[1].length = &sender_length;

    parameters[2].buffer_type = MYSQL_TYPE_STRING;
    parameters[2].buffer = const_cast<char*>(message.recipient.data());
    parameters[2].buffer_length = recipient_length;
    parameters[2].length = &recipient_length;

    parameters[3].buffer_type = MYSQL_TYPE_STRING;
    parameters[3].buffer = const_cast<char*>(message.client_local_id.data());
    parameters[3].buffer_length = local_id_length;
    parameters[3].length = &local_id_length;

    parameters[4].buffer_type = MYSQL_TYPE_STRING;
    parameters[4].buffer = const_cast<char*>(message.content.data());
    parameters[4].buffer_length = content_length;
    parameters[4].length = &content_length;

    parameters[5].buffer_type = MYSQL_TYPE_STRING;
    parameters[5].buffer = const_cast<char*>(message.client_send_at.data());
    parameters[5].buffer_length = client_send_at_length;
    parameters[5].length = &client_send_at_length;

    parameters[6].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[6].buffer = &server_received_at_ms;

    if (mysql_stmt_bind_param(statement.get(),parameters.data()) != 0 ||
        mysql_stmt_execute(statement.get()) != 0)
    {
        return false;
    }

    return mysql_stmt_affected_rows(statement.get()) == 1;

}

StoreOutcome makeExistingMessageOutcome(const NewMessage& message,const ExistingMessageRecord&existing)
{
    if (existing.recipient == message.recipient && existing.content == message.content &&
        existing.client_send_at == message.client_send_at)
    {
        StoreOutcome outcome{};
        outcome.result = StoreResult::DuplicateSame;
        outcome.sender = message.sender;
        outcome.message_id = existing.message_id;
        outcome.recipient = existing.recipient;
        outcome.content = existing.content;
        outcome.client_send_at = existing.client_send_at;
        outcome.server_received_at_ms = existing.server_received_at_ms;
        return outcome;
    }
    return {StoreResult::IdempotencyConflict};
}

} // namespace

MySqlConnectionStatus MySqlMessageRepository::open(const MySqlConnectionConfig &config)
{
    return m_connection.open(config);
}
void MySqlMessageRepository::close() noexcept
{
    m_connection.close();
}
bool MySqlMessageRepository::isOpen() const noexcept
{
    return m_connection.isOpen();
}

bool MySqlMessageRepository::initializeSchema() const
{
    if (!isOpen())
    {
        return false;
    }
    if (!executeSchemaStatement(kCreateSchemaMigrationsTable))
    {
        return false;
    }

    bool version1_applied = false;
    if (!isSchemaVersionApplied(kSchemaVersion1, version1_applied))
    {
        return false;
    }
    if (version1_applied)
    {
        return true;
    }
    if (!applyVersion1())
    {
        return false;
    }

    return recordSchemaVersion(kSchemaVersion1);
}

StoreOutcome MySqlMessageRepository::storeMessage(const NewMessage &message)
{
    if (message.message_id.empty())
    {
        return {StoreResult::DatabaseError};
    }

    MYSQL *const connection = m_connection.nativeHandle();
    if (connection == nullptr || mysql_query(connection, kStartTransactionSql) != 0)
    {
        return {StoreResult::DatabaseError};
    }

    ExistingMessageRecord existing;
    const ExistingMessageLookupResult lookup_result = findExistingMessage(connection, message, true, existing);
    // 查询本身失败：没有可靠业务结果，先尽力结束事务。
    if (lookup_result == ExistingMessageLookupResult::DatabaseError)
    {
        if (mysql_rollback(connection) != 0)
        {
            return {StoreResult::DatabaseError};
        }
        return {StoreResult::DatabaseError};
    }
    // 已存在同一 (sender, client_local_id)：
    // 本分支没有写入，因此回滚用于结束事务并释放 FOR UPDATE 锁
    if (lookup_result == ExistingMessageLookupResult::Found)
    {
        if (mysql_rollback(connection) != 0)
        {
            return {StoreResult::DatabaseError};
        }
       return makeExistingMessageOutcome(message,existing);
    }

    // 这里只有 NotFound：事务仍然开启，尝试写入 Server 提供的候选消息
    if (!insertCandidateMessage(connection,message))
    {
        if (mysql_rollback(connection) != 0)
        {
            return {StoreResult::DatabaseError};
        }
        ExistingMessageRecord recovered;
        if (const auto recovered_result = findExistingMessage(connection, message, false, recovered);
            recovered_result == ExistingMessageLookupResult::Found)
        {
            return makeExistingMessageOutcome(message,recovered);
        }
        return {StoreResult::DatabaseError};
    }

    if (mysql_commit(connection) != 0)
    {
        // commit 失败时持久化结果可能不确定，不能谎报 Stored。
        // 回滚只是尽力清理连接的事务状态。
        mysql_rollback(connection);
        return {StoreResult::DatabaseError};
    }

    // 只有 commit 成功，调用者才能获得确定的 Stored 结果。
    StoreOutcome outcome{};
    outcome.result = StoreResult::Stored;
    outcome.message_id = message.message_id;
    outcome.sender = message.sender;
    outcome.recipient = message.recipient;
    outcome.content = message.content;
    outcome.client_send_at = message.client_send_at;
    outcome.server_received_at_ms = message.server_received_at_ms;
    return outcome;



}

bool MySqlMessageRepository::loadRecentForUser(const std::string &username, const std::optional<HistoryCursor> &before,
                                               int limit, HistoryQueryResult &out_result)
{
    out_result = {};

    MYSQL* const connection = m_connection.nativeHandle();
    if (connection == nullptr || username.empty() ||
        (before.has_value() && before->message_id.empty()))
    {
        return false;
    }

    // 公共合同：页大小最少 1、最多 50；多取一条用于判断是否还有更早历史。
    const int bounded_limit = std::clamp(limit,1,50);
    auto query_limit = static_cast<unsigned int>(bounded_limit + 1);

    const bool has_before = before.has_value();
    const char* const sql = has_before ?
    kSelectRecentMessagesBeforeCursorSql : kSelectRecentMessagesFirstPageSql;

    using StatementOwner = std::unique_ptr<MYSQL_STMT,decltype(&mysql_stmt_close)>;
    const StatementOwner statement{mysql_stmt_init(connection),&mysql_stmt_close};

    // 首页有 3 个 ?；续页有 6 个 ?。必须准备本次实际选中的 SQL。
    if (!statement || mysql_stmt_prepare(statement.get(),sql,
        static_cast<unsigned long>(std::strlen(sql))) != 0)
    {
        return false;
    }

    auto username_length = static_cast<unsigned long>(username.size());

    // mysql_stmt_execute() 执行结束前，绑定变量必须一直存在。
    std::int64_t before_received_at_ms =
        has_before ? before->server_received_at_ms : 0;
    auto before_message_id_length =
        has_before ? static_cast<unsigned long>(before->message_id.size()) : 0;

    std::array<MYSQL_BIND,6> parameters{};
    parameters[0].buffer_type = MYSQL_TYPE_STRING;
    parameters[0].buffer = const_cast<char*>(username.data());
    parameters[0].buffer_length = username_length;
    parameters[0].length = &username_length;

    parameters[1].buffer_type = MYSQL_TYPE_STRING;
    parameters[1].buffer = const_cast<char*>(username.data());
    parameters[1].buffer_length = username_length;
    parameters[1].length = &username_length;

    if (!has_before)
    {
        // 首页第三个 ?：LIMIT limit + 1。
        parameters[2].buffer_type = MYSQL_TYPE_LONG;
        parameters[2].buffer = &query_limit;
        parameters[2].is_unsigned = 1;
    }else
    {
        // 续页 SQL 的第三、第四个 ?：游标时间。
        parameters[2].buffer_type = MYSQL_TYPE_LONGLONG;
        parameters[2].buffer = &before_received_at_ms;

        parameters[3].buffer_type = MYSQL_TYPE_LONGLONG;
        parameters[3].buffer = &before_received_at_ms;

        // 第五个 ?：同一毫秒时，用 message_id 排除游标自身及较新的记录。
        parameters[4].buffer_type = MYSQL_TYPE_STRING;
        parameters[4].buffer =
            const_cast<char*>(before->message_id.data());
        parameters[4].buffer_length = before_message_id_length;
        parameters[4].length = &before_message_id_length;

        // 第六个 ?：LIMIT limit + 1。
        parameters[5].buffer_type = MYSQL_TYPE_LONG;
        parameters[5].buffer = &query_limit;
        parameters[5].is_unsigned = 1;
    }

    if (mysql_stmt_bind_param(statement.get(),parameters.data()) != 0 ||
        mysql_stmt_execute(statement.get()) != 0 ||
        mysql_stmt_store_result(statement.get()) != 0)
    {
        return false;
    }

    // MySQL C API 不会直接创建 std::string；必须提供每列的接收缓冲区。
    std::array<char, 32> message_id_buffer{};
    std::array<char, 20> sender_buffer{};
    std::array<char, 20> recipient_buffer{};
    std::array<char, 64> client_local_id_buffer{};
    std::array<char, 1024> content_buffer{};
    std::array<char, 64> client_send_at_buffer{};
    std::int64_t server_received_at_ms{};

    // lengths 是每列实际字节数；is_null/errors 用于拒绝异常行或截断行。
    std::array<unsigned long, 7> lengths{};
    std::array<my_bool, 7> is_null{};
    std::array<my_bool, 7> errors{};
    std::array<MYSQL_BIND, 7> results{};

    results[0].buffer_type = MYSQL_TYPE_STRING;
    results[0].buffer = message_id_buffer.data();
    results[0].buffer_length =
        static_cast<unsigned long>(message_id_buffer.size());
    results[0].length = &lengths[0];
    results[0].is_null = &is_null[0];
    results[0].error = &errors[0];

    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = sender_buffer.data();
    results[1].buffer_length =
        static_cast<unsigned long>(sender_buffer.size());
    results[1].length = &lengths[1];
    results[1].is_null = &is_null[1];
    results[1].error = &errors[1];

    results[2].buffer_type = MYSQL_TYPE_STRING;
    results[2].buffer = recipient_buffer.data();
    results[2].buffer_length =
        static_cast<unsigned long>(recipient_buffer.size());
    results[2].length = &lengths[2];
    results[2].is_null = &is_null[2];
    results[2].error = &errors[2];

    results[3].buffer_type = MYSQL_TYPE_STRING;
    results[3].buffer = client_local_id_buffer.data();
    results[3].buffer_length =
        static_cast<unsigned long>(client_local_id_buffer.size());
    results[3].length = &lengths[3];
    results[3].is_null = &is_null[3];
    results[3].error = &errors[3];

    results[4].buffer_type = MYSQL_TYPE_STRING;
    results[4].buffer = content_buffer.data();
    results[4].buffer_length =
        static_cast<unsigned long>(content_buffer.size());
    results[4].length = &lengths[4];
    results[4].is_null = &is_null[4];
    results[4].error = &errors[4];

    results[5].buffer_type = MYSQL_TYPE_STRING;
    results[5].buffer = client_send_at_buffer.data();
    results[5].buffer_length =
        static_cast<unsigned long>(client_send_at_buffer.size());
    results[5].length = &lengths[5];
    results[5].is_null = &is_null[5];
    results[5].error = &errors[5];

    results[6].buffer_type = MYSQL_TYPE_LONGLONG;
    results[6].buffer = &server_received_at_ms;
    results[6].is_null = &is_null[6];
    results[6].error = &errors[6];

    if (mysql_stmt_bind_result(statement.get(), results.data()) != 0)
    {
        return false;
    }

    // page 是临时结果：只有整页成功时，才赋给 out_result。
    HistoryQueryResult page;
    while (true)
    {
        const int fetch_result = mysql_stmt_fetch(statement.get());
        if (fetch_result == MYSQL_NO_DATA)
        {
            break;
        }
        if (fetch_result != 0)
        {
            return false;
        }
        for (const my_bool error : errors)
        {
            if (error)
            {
                return false;
            }
        }
        for (const my_bool null_value : is_null)
        {
            if (null_value)
            {
                return false;
            }
        }

        StoredMessage message;
        // 使用数据库报告的实际长度复制，不能依赖缓冲区末尾的 '\0'。
        message.message_id =
            std::string(message_id_buffer.data(), lengths[0]);
        message.sender =
            std::string(sender_buffer.data(), lengths[1]);
        message.recipient =
            std::string(recipient_buffer.data(), lengths[2]);
        message.client_local_id =
            std::string(client_local_id_buffer.data(), lengths[3]);
        message.content =
            std::string(content_buffer.data(), lengths[4]);
        message.client_send_at =
            std::string(client_send_at_buffer.data(), lengths[5]);
        message.server_received_at_ms = server_received_at_ms;

        page.messages.push_back(message);
    }

    // SQL 是新到旧；第 limit + 1 条只用于判断是否还有更旧数据。
    page.has_more = page.messages.size() > static_cast<std::size_t>(bounded_limit);
    if (page.has_more)
    {
        page.messages.pop_back();
    }

    // 共同合同要求页面输出旧到新。
    std::reverse(page.messages.begin(), page.messages.end());

    // 有更早一页时，游标指向本页最旧消息。
    if (page.has_more)
    {
        const StoredMessage& oldest_message = page.messages.front();
        page.next_cursor = HistoryCursor{ oldest_message.server_received_at_ms,oldest_message.message_id};
    }

    out_result = page;
    return true;

}

bool MySqlMessageRepository::isSchemaVersionApplied(const int version, bool &out_applied) const
{
    out_applied = false;

    MYSQL *connection = m_connection.nativeHandle();
    if (connection == nullptr || version <= 0)
    {
        return false;
    }

    const std::string query = "SELECT 1 FROM schema_migrations WHERE version = " + std::to_string(version) + " LIMIT 1";
    if (mysql_query(connection, query.c_str()) != 0)
    {
        return false;
    }

    using ResultOwner = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;
    const ResultOwner result{mysql_store_result(connection), &mysql_free_result};
    if (!result)
    {
        return false;
    }

    out_applied = mysql_num_rows(result.get()) != 0;
    return true;
}
bool MySqlMessageRepository::applyVersion1() const
{
    return executeSchemaStatement(kCreateMessagesTableV1);
}

bool MySqlMessageRepository::recordSchemaVersion(const int version) const
{
    if (version <= 0)
    {
        return false;
    }

    const std::string statement = "INSERT INTO schema_migrations (version) VALUES (" + std::to_string(version) + ")";

    return executeSchemaStatement(statement.c_str());
}

bool MySqlMessageRepository::executeSchemaStatement(const char *sql) const
{
    MYSQL *connection = m_connection.nativeHandle();

    if (connection == nullptr || sql == nullptr || *sql == '\0')
    {
        return false;
    }
    return mysql_query(connection, sql) == 0;
}
} // namespace repository
