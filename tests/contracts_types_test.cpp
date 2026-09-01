// M1-2 强类型与 Outcome 单元测试：每个 CHECK 对应一条合同（D44/D128/D209/D211）。
// 这里不复刻解析算法，只给出输入和期望的公共行为。

#include <chathub/contracts/ids.hpp>
#include <chathub/contracts/outcome.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

using namespace chathub::contracts;

#define CHECK(expr)                                          \
    do {                                                     \
        if (!(expr)) {                                       \
            /* 返回非零，使 CTest 将当前可执行测试标为失败。 */ \
            std::fputs("CHECK failed: " #expr "\n", stderr); \
            return 1;                                        \
        }                                                    \
    } while (false)

static int test_account_name()
{
    // 覆盖 D44 的成功、长度边界、首字符和字符集，并检查规范化后的可比较值。
    CHECK(AccountName::parse("abcd").has_value());
    CHECK(AccountName::parse("Abcd01").has_value());
    CHECK(AccountName::parse(std::string(32, 'a')).has_value());
    CHECK(!AccountName::parse("abc").has_value());                  // 3 字符（D44 下界）
    CHECK(!AccountName::parse(std::string(33, 'a')).has_value());   // 33 字符（上界）
    CHECK(!AccountName::parse("1abc").has_value());                 // 数字开头
    CHECK(!AccountName::parse("ab-c").has_value());                 // 非字母数字
    CHECK(AccountName::parse("AbCd")->value() == "abcd");      // 小写规范化（D44）
    return 0;
}

static int test_uuid_ids()
{
    // 同时验证 UUID 形状、大小写规范化与三个强类型可独立构造。
    const std::string lower = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
    CHECK(MessageId::parse(lower).has_value());
    CHECK(MessageId::parse("0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0").has_value());
    CHECK(MessageId::parse("0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0")->value() == lower);
    CHECK(!MessageId::parse("").has_value());
    CHECK(!MessageId::parse("0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1fg").has_value());  // 非 hex
    CHECK(!MessageId::parse("0f1e2d3-c4b5a-6978-8796-a5b4c3d2e1f0").has_value());  // 分组错位
    CHECK(!MessageId::parse("0f1e2d3c-4b5a-6978-8796-a5b4c3d2 1f0").has_value());  // 非法字符
    CHECK(LocalMessageId::parse(lower).has_value());
    CHECK(RequestId::parse(lower).has_value());
    return 0;
}

static int test_seqs()
{
    // 验证规范十进制：完整消费输入、拒绝前导零与 uint64 溢出。
    CHECK(DeliverySeq::parse("0").has_value());
    CHECK(DeliverySeq::parse("42")->value() == 42);
    CHECK(DeliverySeq::parse("42")->to_decimal_string() == "42");
    CHECK(DeliverySeq::of(1'048'576).to_decimal_string() == "1048576");
    CHECK(!DeliverySeq::parse("").has_value());
    CHECK(!DeliverySeq::parse("+42").has_value());
    CHECK(!DeliverySeq::parse("007").has_value());                    // 禁止前导零
    CHECK(!DeliverySeq::parse("4 2").has_value());
    CHECK(!DeliverySeq::parse("18446744073709551616").has_value());   // uint64 溢出
    CHECK(DeliverySeq::parse("18446744073709551615").has_value());    // 上界
    CHECK(ConversationSeq::parse("7").has_value());
    return 0;
}

static int test_outcome()
{
    // 成功和预期失败共用 Outcome，但调用方可通过 is_ok 选择正确分支。
    const auto ok = Outcome<int>{42};
    CHECK(is_ok(ok));
    CHECK(value(ok) == 42);

    const auto bad = Outcome<int>{Error{.code = "invalid_message", .detail = "测试"}};
    CHECK(!is_ok(bad));
    CHECK(error(bad).code == "invalid_message");
    CHECK(is_snake_case_code("invalid_message"));
    CHECK(!is_snake_case_code("Invalid-Message"));
    return 0;
}

static int test_no_implicit_conversion()
{
    // 这些是编译期断言；运行到函数本身不产生业务行为。
    static_assert(!std::is_convertible_v<MessageId, LocalMessageId>);
    static_assert(!std::is_convertible_v<LocalMessageId, MessageId>);
    static_assert(!std::is_convertible_v<RequestId, MessageId>);
    static_assert(!std::is_convertible_v<DeliverySeq, ConversationSeq>);
    static_assert(!std::is_convertible_v<MessageId, std::string>);
    return 0;
}

int main()
{
    // 遇到首个失败立即返回，CTest 会显示对应 CHECK 的表达式。
    if (const int rc = test_account_name()) { return rc; }
    if (const int rc = test_uuid_ids()) { return rc; }
    if (const int rc = test_seqs()) { return rc; }
    if (const int rc = test_outcome()) { return rc; }
    if (const int rc = test_no_implicit_conversion()) { return rc; }
    return 0;
}
