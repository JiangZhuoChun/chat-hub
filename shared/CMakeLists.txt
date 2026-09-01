add_library(chathub_contracts INTERFACE)
add_library(chathub::contracts ALIAS chathub_contracts)

# 这是 header-only 公共合同；消费者链接它后必须以 C++20 编译。
target_compile_features(chathub_contracts INTERFACE cxx_std_20)

# 对外暴露 shared/include，使消费者可写 <chathub/contracts/...>。
target_include_directories(chathub_contracts INTERFACE include)

# 即使暂时没有 .cpp 包含这些头，仍把它们归属到 contracts target，
# 让 CLion 使用该 target 的 C++20 和标准库上下文解析它们。
target_sources(chathub_contracts INTERFACE
        include/chathub/contracts/ids.hpp
        include/chathub/contracts/outcome.hpp
        include/chathub/contracts/protocol_descriptor.hpp
        include/chathub/contracts/frame.hpp
)
