#pragma once

#include "repository/message_repository_contract.h"
#include "repository/mysql_connection.h"

namespace repository
{
class MySqlMessageRepository final : public IMessageRepository
{
public:
    MySqlMessageRepository() = default;
    ~MySqlMessageRepository() override = default;

    MySqlMessageRepository(const MySqlMessageRepository&) = delete;
    MySqlMessageRepository& operator=(const MySqlMessageRepository&) = delete;

    MySqlConnectionStatus open(const MySqlConnectionConfig& config);
    void close() noexcept;
    bool isOpen() const noexcept;

    // 仅供 MySQL Factory 或 MySQL 测试夹具初始化私有 Schema。
    bool initializeSchema() const;

    StoreOutcome storeMessage(const NewMessage& message) override;
    bool loadRecentForUser(const std::string& username,
                            const std::optional<HistoryCursor>& before,
                            int limit,
                            HistoryQueryResult& out_result) override;

private:
    static constexpr int kSchemaVersion1 = 1;

    bool isSchemaVersionApplied(int version, bool& out_applied) const;
    bool applyVersion1() const;
    bool recordSchemaVersion(int version) const;
    bool executeSchemaStatement(const char* sql) const;

    MySqlConnection m_connection;
};
}
