#pragma once
#include "repository/message_repository_contract.h"

#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string>
namespace repository
{

std::unique_ptr<IMessageRepository> createSqliteMessageRepository(const std::string &db_path);
class SqliteMessageRepository final : public IMessageRepository
{
  public:
    SqliteMessageRepository() = default;
    ~SqliteMessageRepository() override;

    SqliteMessageRepository(const SqliteMessageRepository &) = delete;
    SqliteMessageRepository &operator=(const SqliteMessageRepository &) = delete;

    // 打开/迁移数据库；返回 true 表示可用
    bool open(const std::string &db_path);

    // 存储消息；重复时按幂等规则返回既有记录或冲突
    StoreOutcome storeMessage(const NewMessage &message) override;

    // 加载某用户参与的最近消息（按 server_received_at_ms 排序）
    // 返回完整查询结果
    bool loadRecentForUser(const std::string &username, const std::optional<HistoryCursor> &before, int limit,
                           HistoryQueryResult &out_result) override;

  private:
    bool exec(const char *sql);

    void close();

    void log(const std::string &operation) const;

    sqlite3 *m_db = nullptr;
};
} // namespace repository
