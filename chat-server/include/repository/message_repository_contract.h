#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace repository
{
// 存储结果枚举（storeOrGetExisting 的返回值）
enum class StoreResult
{
    Stored,              // 新插入成功
    DuplicateSame,       // 完全重复: 同一 sender+local_id，正文也相同
    IdempotencyConflict, // 冲突: 同一local_id，但正文、接收者、时间不同
    DatabaseError        // 数据库失败
};
// 持久记录结构（StoreOutcome——带状态 + 完整持久数据）
struct StoreOutcome
{
    StoreResult result;
    // 持久记录字段(result != DatabaseError时有效)
    std::string message_id; // 服务端 UUID（Stored 时新生成，DuplicateSame 时既有）
    std::string sender;
    std::string recipient;
    std::string content;
    std::string client_send_at;
    std::int64_t server_received_at_ms;
};
// 历史记录结构（loadRecentForUser 返回的单条
struct StoredMessage
{
    std::string message_id;
    std::string sender;
    std::string recipient;
    std::string content;
    std::string client_send_at;
    std::string client_local_id;
    std::int64_t server_received_at_ms;
};
// 历史记录游标结构
struct HistoryCursor
{
    std::int64_t server_received_at_ms;
    std::string message_id;
};
// 历史记录查询结果结构（loadRecentForUser 返回的完整结构）
struct HistoryQueryResult
{
    std::vector<StoredMessage> messages;      // 本页消息
    bool has_more{false};                     // 是否存在更早一页
    std::optional<HistoryCursor> next_cursor; // 下一页游标（如果 has_more == true）
};
// 封装一次发送请求的完整参数
struct NewMessage
{
    std::string sender;
    std::string recipient;
    std::string content;
    std::string client_send_at;
    std::string client_local_id;
    std::int64_t server_received_at_ms;
    std::string message_id;
};

class IMessageRepository
{
  public:
    virtual ~IMessageRepository() = default;

    virtual StoreOutcome storeMessage(const NewMessage &message) = 0;
    virtual bool loadRecentForUser(const std::string &username, const std::optional<HistoryCursor> &before, int limit,
                                   HistoryQueryResult &out_result) = 0;
};

} // namespace repository
