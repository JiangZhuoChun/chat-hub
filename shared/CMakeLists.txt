# 公共合同层：仅包含头文件，并向所有消费者传递 C++20 要求。
add_library(chathub_contracts INTERFACE)
add_library(chathub::contracts ALIAS chathub_contracts)
target_compile_features(chathub_contracts INTERFACE cxx_std_20)

# 公开头路径：消费者统一使用 <chathub/contracts/...> 引入合同。
target_include_directories(chathub_contracts INTERFACE include)

# IDE 归属：显式列出 header-only 源，使 CLion 使用 contracts 的解析上下文。
target_sources(chathub_contracts INTERFACE
        include/chathub/contracts/ids.hpp
        include/chathub/contracts/outcome.hpp
        include/chathub/contracts/protocol_descriptor.hpp
        include/chathub/contracts/frame.hpp
        include/chathub/contracts/hello.hpp
)
