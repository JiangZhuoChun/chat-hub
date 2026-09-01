#include "repository/sqlite_message_repository.h"

#include <algorithm>
#include <iostream>
#include <sqlite3.h>
#include <string>

namespace repository
{
std::unique_ptr<IMessageRepository> createSqliteMessageRepository(const std::string &db_path)
{
    auto repo = std::make_unique<SqliteMessageRepository>();

    if (!repo->open(db_path))
    {
        return nullptr;
    }
    return repo;
}

SqliteMessageRepository::~SqliteMessageRepository()
{
    close();
}

bool SqliteMessageRepository::open(const std::string &db_path)
{
    int rc = sqlite3_open(db_path.c_str(), &m_db);

    if (rc != SQLITE_OK)
    {
        // 功能：打开失败时用 rc 直接取错误文本，避免在 m_db 可能无效时调用 sqlite3_errmsg(m_db)
        std::cerr << "failed to open database: " << sqlite3_errstr(rc) << std::endl;
        close();
        return false;
    }
    // 第二步：设置 busy_timeout
    rc = sqlite3_busy_timeout(m_db, 3000);
    if (rc != SQLITE_OK)
    {
        log("failed to set busy timeout");
        close();
        return false;
    }
    // 第三步：建表
    const auto *creat_table_sql = R"(
            CREATE TABLE IF NOT EXISTS messages (
                message_id             TEXT PRIMARY KEY,
                sender                 TEXT NOT NULL,
                recipient              TEXT NOT NULL,
                participant_low        TEXT NOT NULL,
                participant_high       TEXT NOT NULL,
                client_local_id        TEXT NOT NULL,
                content                TEXT NOT NULL,
                client_send_at         TEXT NOT NULL,
                server_received_at_ms  INTEGER NOT NULL,
                UNIQUE(sender, client_local_id)
            );
        )";
    if (!exec(creat_table_sql))
    {
        close();
        return false;
    }
    // 第四步：创建索引
    const auto *create_indexes_sql = R"(
            CREATE INDEX IF NOT EXISTS idx_messages_conversation_order
                 ON messages(
                    participant_low,
                    participant_high,
                    server_received_at_ms DESC,
                    message_id DESC
                );
            CREATE INDEX IF NOT EXISTS idx_messages_user_order
                ON messages(
                    sender,
                    server_received_at_ms DESC,
                    message_id DESC
                );
            CREATE INDEX IF NOT EXISTS idx_messages_recipient_order
                ON messages(
                     recipient,
                     server_received_at_ms DESC,
                     message_id DESC
                );
        )";
    if (!exec(create_indexes_sql))
    {
        close();
        return false;
    }
    // 第五步：设置数据库版本
    if (!exec("PRAGMA user_version = 1"))
    {
        close();
        return false;
    }
    return true;
}

// 存储或获取现有消息
// 功能：按 UNIQUE(sender, client_local_id) 幂等规则写入或复用一条持久消息。
// 返回：Stored（新插入）/ DuplicateSame（完全重复，复用既有记录）/
//       IdempotencyConflict（同一 local_id 但内容不同）/ DatabaseError。
StoreOutcome SqliteMessageRepository::storeMessage(const NewMessage &message)
{
    if (message.message_id.empty())
    {
        log("missing candidate message id");
        return StoreOutcome{StoreResult::DatabaseError};
    }
    // ===== 第 1 步：查幂等记录（sender + client_local_id）=====
    sqlite3_stmt *stmt = nullptr;
    const auto *sql = R"(
            SELECT message_id, recipient, content, client_send_at, server_received_at_ms
            FROM messages WHERE sender=? AND client_local_id=?
        )";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        log("failed to prepare select statement");
        return StoreOutcome{StoreResult::DatabaseError};
    }
    // 绑定查询参数（索引从 1 开始）
    sqlite3_bind_text(stmt, 1, message.sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, message.client_local_id.c_str(), -1, SQLITE_TRANSIENT);

    // 执行查询：SQLITE_ROW=查到旧记录，SQLITE_DONE=无记录
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    {
        log("failed to step select statement");
        sqlite3_finalize(stmt);
        return StoreOutcome{StoreResult::DatabaseError};
    }

    // ===== 第 2 步：查到了旧记录 → 判断是完全重复还是冲突 =====
    if (rc == SQLITE_ROW)
    {
        // 取出旧记录各列（column 索引从 0 开始）
        const auto *old_recipient = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        const auto *old_content = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        const auto *old_client_send_at = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        const auto old_server_received_at_ms = sqlite3_column_int64(stmt, 4);

        const std::string old_recipient_str = old_recipient ? old_recipient : "";
        const std::string old_content_str = old_content ? old_content : "";
        const std::string old_client_send_at_str = old_client_send_at ? old_client_send_at : "";

        // 完全重复判定：recipient/content/client_send_at 三个字段都一致。
        // 注意：server_received_at_ms 是服务端生成的可信时间，不参与比较。
        const bool same = (old_recipient_str == message.recipient && old_content_str == message.content &&
                           old_client_send_at_str == message.client_send_at);

        // 情况 A：完全重复 → 复用既有记录（返回 DuplicateSame + 旧 message_id）
        if (same)
        {
            const auto *old_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            StoreOutcome out;
            out.result = StoreResult::DuplicateSame;
            out.message_id = old_id ? old_id : "";
            out.sender = message.sender;
            out.recipient = message.recipient;
            out.content = old_content_str;
            out.client_send_at = old_client_send_at_str;
            out.server_received_at_ms = old_server_received_at_ms;
            sqlite3_finalize(stmt);
            return out;
        }
        // 情况 B：同一 local_id 但内容不同 → 幂等冲突，拒绝写入
        sqlite3_finalize(stmt);
        return StoreOutcome{StoreResult::IdempotencyConflict};
    }

    // 无既有幂等记录：持久化 Server 已生成的候选 ID。
    const std::string& message_id = message.message_id;

    // 会话归属：两个用户名按字典序，low 在前 high 在后（服务端算，不信客户端）
    const std::string low = (message.sender < message.recipient) ? message.sender : message.recipient;
    const std::string high = (message.sender < message.recipient) ? message.recipient : message.sender;

    // 预编译插入语句（9 个字段，? 占位符）
    sqlite3_stmt *ins_stmt = nullptr;
    const auto *insert_sql = R"(
            INSERT INTO messages
            (message_id, sender, recipient, participant_low, participant_high,
             client_local_id, content, client_send_at, server_received_at_ms)
            VALUES (?,?,?,?,?,?,?,?,?)
        )";
    if (sqlite3_prepare_v2(m_db, insert_sql, -1, &ins_stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        log("failed to prepare insert statement");
        return StoreOutcome{StoreResult::DatabaseError};
    }
    // 绑定 9 个插入值（索引从 1 开始）
    sqlite3_bind_text(ins_stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 2, message.sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 3, message.recipient.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 4, low.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 5, high.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 6, message.client_local_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 7, message.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 8, message.client_send_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins_stmt, 9, message.server_received_at_ms);

    // 执行插入：SQLITE_DONE=成功，其他（如 UNIQUE 冲突）为失败
    const int irc = sqlite3_step(ins_stmt);
    sqlite3_finalize(ins_stmt);
    sqlite3_finalize(stmt);

    if (irc == SQLITE_DONE)
    {
        StoreOutcome out;
        out.result = StoreResult::Stored;
        out.message_id = message_id;
        out.sender = message.sender;
        out.recipient = message.recipient;
        out.content = message.content;
        out.client_send_at = message.client_send_at;
        out.server_received_at_ms = message.server_received_at_ms;
        return out;
    }

    log("failed to insert message");
    return StoreOutcome{StoreResult::DatabaseError};
}

// 加载用户最近消息
// 功能：加载用户最近消息，可选地指定复合游标，在该时间戳之前的消息全部加载出来。
bool SqliteMessageRepository::loadRecentForUser(const std::string &username, const std::optional<HistoryCursor> &before,
                                                int limit, HistoryQueryResult &out_result)
{
    out_result = {};
    if (m_db == nullptr || username.empty())
    {
        return false;
    }
    // 第一页 SQL
    const auto *sql_first_page = R"(
            SELECT
                message_id, sender, recipient, client_local_id, content, client_send_at, server_received_at_ms
            FROM messages
            WHERE sender = ? or recipient = ?
            ORDER BY server_received_at_ms DESC, message_id DESC
            LIMIT ? ;
        )";
    // 为 before 设计第二份 SQL，并根据 before 选择 SQL
    const auto *sql_with_before = R"(
                SELECT
                    message_id,sender,recipient,client_local_id,content,client_send_at,server_received_at_ms
                FROM messages
                WHERE (sender = ? OR recipient = ?)
                  AND ( server_received_at_ms < ?
                      OR (server_received_at_ms = ?AND message_id < ?)
                       )
                ORDER BY server_received_at_ms DESC, message_id DESC
                LIMIT ?;
        )";
    const auto *sql = before.has_value() ? sql_with_before : sql_first_page;
    const int bounded_limit = std::clamp(limit, 1, 50);
    sqlite3_stmt *stmt = nullptr;

    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        log("Failed to prepare initial message query:");
        return false;
    }
    // 公共部分:绑定用户名
    rc = sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        log("Failed to bind sender username:");
        sqlite3_finalize(stmt);
        return false;
    }
    rc = sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        log("Failed to bind recipient username:");
        sqlite3_finalize(stmt);
        return false;
    }

    // 带 before
    if (before.has_value())
    {
        rc = sqlite3_bind_int64(stmt, 3, before->server_received_at_ms);
        if (rc != SQLITE_OK)
        {
            log("Failed to bind before timestamp:");
            sqlite3_finalize(stmt);
            return false;
        }
        rc = sqlite3_bind_int64(stmt, 4, before->server_received_at_ms);
        if (rc != SQLITE_OK)
        {
            log("Failed to bind before timestamp:");
            sqlite3_finalize(stmt);
            return false;
        }
        rc = sqlite3_bind_text(stmt, 5, before->message_id.c_str(), -1, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK)
        {
            log("Failed to bind before message ID:");
            sqlite3_finalize(stmt);
            return false;
        }
        rc = sqlite3_bind_int(stmt, 6, bounded_limit + 1);
        if (rc != SQLITE_OK)
        {
            log("Failed to bind limit:");
            sqlite3_finalize(stmt);
            return false;
        }
    }
    // 首屏
    else
    {
        rc = sqlite3_bind_int(stmt, 3, bounded_limit + 1);
        if (rc != SQLITE_OK)
        {
            log("Failed to bind limit:");
            sqlite3_finalize(stmt);
            return false;
        }
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        StoredMessage message;
        // sqlite3_column_text() 返回的不是 std::string，而是 SQLite 内部管理的一块内存
        // 这个指针的有效期和当前 stmt、当前结果行有关
        const auto *message_id_text = sqlite3_column_text(stmt, 0);
        const auto *sender_text = sqlite3_column_text(stmt, 1);
        const auto *recipient_text = sqlite3_column_text(stmt, 2);
        const unsigned char *client_local_id_text = sqlite3_column_text(stmt, 3);
        const unsigned char *content_text = sqlite3_column_text(stmt, 4);
        const unsigned char *client_send_at_text = sqlite3_column_text(stmt, 5);

        // 文字列都立即复制为 std::string，没有保存 SQLite 临时指针
        message.message_id = message_id_text ? reinterpret_cast<const char *>(message_id_text) : "";
        message.sender = sender_text ? reinterpret_cast<const char *>(sender_text) : "";
        message.recipient = recipient_text ? reinterpret_cast<const char *>(recipient_text) : "";
        message.client_local_id = client_local_id_text ? reinterpret_cast<const char *>(client_local_id_text) : "";
        message.content = content_text ? reinterpret_cast<const char *>(content_text) : "";
        message.client_send_at = client_send_at_text ? reinterpret_cast<const char *>(client_send_at_text) : "";
        message.server_received_at_ms = sqlite3_column_int64(stmt, 6);

        out_result.messages.push_back(message);
    }
    if (rc != SQLITE_DONE)
    {
        log("Failed to step message query");
        sqlite3_finalize(stmt);
        return false;
    }
    // 把“多取一条”的数据库结果转成正确的历史页
    out_result.has_more = (out_result.messages.size() > static_cast<std::size_t>(bounded_limit));
    // 如果 has_more，删除最后那条额外记录
    if (out_result.has_more)
    {
        out_result.messages.pop_back();
    }

    // 无条件反转messages，让客户端最终得到旧到新顺序
    std::reverse(out_result.messages.begin(), out_result.messages.end());

    // 如果 has_more，从 messages.front() 生成 next_cursor
    if (out_result.has_more)
    {
        const auto message_id = out_result.messages.front().message_id;
        const auto server_received_at_ms = out_result.messages.front().server_received_at_ms;
        out_result.next_cursor = HistoryCursor{server_received_at_ms, message_id};
    }

    sqlite3_finalize(stmt);
    return true;
}

bool SqliteMessageRepository::exec(const char *sql)
{
    char *error_message = nullptr;

    if (const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &error_message); rc != SQLITE_OK)
    {
        std::cerr << "SQLite error:" << (error_message ? error_message : "unknown") << std::endl;
        // 字符串是 SQLite 分配的，所以用完必须
        sqlite3_free(error_message);
        return false;
    }
    return true;
}

void SqliteMessageRepository::close()
{
    if (m_db != nullptr)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void SqliteMessageRepository::log(const std::string &operation) const
{
    std::cerr << operation << ":" << sqlite3_errmsg(m_db) << std::endl;
}
} // namespace repository
