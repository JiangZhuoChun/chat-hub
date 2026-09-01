#include "repository/mysql_message_repository.h"

#include <cstdlib>
#include <iostream>
#include <type_traits>

//1. 你真的是 IMessageRepository 的实现吗？
// 2. 你允许被复制构造吗？
// 3. 你允许被复制赋值吗？
static_assert(std::is_base_of_v<repository::IMessageRepository,
                                repository::MySqlMessageRepository>);

static_assert(!std::is_copy_constructible_v<repository::MySqlMessageRepository>);

static_assert(!std::is_copy_assignable_v<repository::MySqlMessageRepository>);

int main()
{
    std::cout << "PASS [mysql-repository-declaration]";
}