# M0-3 PRP：最小 CTest smoke 闭环与 test presets

> 状态：已交付（2026-09-01）。CTest 1/1 通过、0 失败 0 跳过（用户回报 1.06s；本机复跑 0.40s，退出码 0，标签 smoke／contracts 生效）；`tests/` 为唯一新增目录；`noTestsAction: "error"` 本机实证生效。落地样式（缩进／括号）由用户按 CLion 风格调整，行为与 PRP 等价。

## 1. 目标与边界

M0-3 让新工程从“可 configure／build”变为“可 configure／build／CTest”。它创建一个最小消费者 target，链接 `chathub::contracts`，使用 C++20 `<concepts>` 静态断言并以 0 退出；CTest 运行该程序并报告结果。

这只证明构建图、C++20 编译特性传播、二进制启动和 CTest 测试发现的闭环。它不证明任何聊天功能、网络、TLS、Qt 或业务合同已经存在。

## 2. 本步新概念与调用链

```text
include(CTest) → BUILD_TESTING=ON → enable_testing()
→ add_subdirectory(tests) → add_executable(smoke)
→ target_link_libraries(smoke PRIVATE chathub::contracts)
→ add_test(NAME … COMMAND smoke) → CTestTestfile.cmake
→ ctest --preset windows-mingw-debug → 进程退出码 → 测试报告
```

- `include(CTest)`：顶层测试开关；默认创建 `BUILD_TESTING=ON` 并调用 `enable_testing()`。它不会自动产生测试。
- `add_executable`：本步不是 Qt 应用，故使用标准 CMake 创建仅供测试的控制台进程。
- `target_link_libraries(... PRIVATE chathub::contracts)`：让消费者继承公共合同的 `cxx_std_20`；`PRIVATE` 表示 smoke 自身使用该合同，不向其他 target 暴露。
- `add_test`：把已构建的可执行 target 名称映射为 CTest 测试记录；没有它，二进制存在也不会被 CTest 运行。
- `testPresets`：将 CTest 指向与 configure preset 相同的构建目录；`noTestsAction: error` 将“没有发现测试”变成非零退出，阻止假绿。

## 3. 确认后的最小文件与精确内容

| 路径 | 变更 | 目的 |
| --- | --- | --- |
| `CMakeLists.txt` | 在 `add_subdirectory(shared)` 后，以 `if(BUILD_TESTING)` 包裹 `add_subdirectory(tests)` | 测试关闭时不加载测试目录 |
| `tests/CMakeLists.txt` | 声明 `chathub_contracts_smoke`；私有链接 `chathub::contracts`；注册 `chathub.contracts.smoke` 并标记 `smoke;contracts` | 一个真实消费者和一个 CTest 条目 |
| `tests/contracts_smoke.cpp` | `<concepts>` 的 C++20 静态断言与 `int main() { return 0; }` | 编译期验证 C++20，运行期验证启动 |
| `CMakePresets.json` | 增加 Debug、RelWithDebInfo、Release 同名 `testPresets`；每个设置 `outputOnFailure: true`、`stopOnFailure: true`、`noTestsAction: error` | 可复现且不会“0 tests”假通过 |

唯一应新增的目录是 `tests/`；不建立 `client-qt/`、`chat-server/` 或任何占位目录。

## 4. 精确 CMake 骨架

根 CMake 的测试入口：

```cmake
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

`tests/CMakeLists.txt` 的 target／注册关系：

```cmake
add_executable(chathub_contracts_smoke contracts_smoke.cpp)

target_link_libraries(chathub_contracts_smoke PRIVATE
    chathub::contracts
)

add_test(NAME chathub.contracts.smoke COMMAND chathub_contracts_smoke)
set_tests_properties(chathub.contracts.smoke PROPERTIES LABELS "smoke;contracts")
```

测试源的最小行为（无输入、无输出）：

```cpp
#include <concepts>

static_assert(std::same_as<int, int>);

int main()
{
    return 0;
}
```

若 `chathub::contracts` 没有把 C++20 特性传播给消费者，`<concepts>`／`std::same_as` 的编译会失败；若程序不能启动或返回非零，CTest 失败。

## 5. Preset 结构

在现有 `buildPresets` 后新增 `testPresets` 数组。三个条目分别关联同名 configure preset；共同语义如下：

```json
{
  "name": "windows-mingw-debug",
  "configurePreset": "windows-mingw-debug",
  "output": { "outputOnFailure": true },
  "execution": {
    "stopOnFailure": true,
    "noTestsAction": "error"
  }
}
```

RelWithDebInfo 与 Release 仅替换两个 preset 名称。configure、build、test preset 同名是 CMake 允许的不同命名空间，不是冲突。

> 实证（2026-09-01，本机 CMake 4.3.2，schema v4）：`noTestsAction: "error"` 生效——无测试时 `ctest --preset` 退出码 8 并报 "Errors while running CTest"；移除该字段后默认退出码 0。未知或拼错的 preset 字段会被静默忽略，必须使用准确拼写。

## 6. 实施顺序与失败边界

1. 创建 `tests/`，先写 smoke 源和该目录的 CMake；
2. 在根 CMake 增加受 `BUILD_TESTING` 保护的 `add_subdirectory(tests)`；
3. 补齐三个 test presets；
4. 通过 fresh Debug build 执行 configure、build、CTest；
5. 只在所有命令成功后更新本 PRP／TASK／INITIAL 的实施证据。

| 现象 | 最小处理 |
| --- | --- |
| `tests` 不存在或没有 CMakeLists | 根 CMake 的 `add_subdirectory(tests)` 配置失败；核对目录／文件，而不是删除测试入口 |
| `BUILD_TESTING=OFF` | 不会创建测试 target；CTest 的 `noTestsAction: error` 应以非零退出；仅为明确关闭测试的场景允许此失败 |
| `<concepts>` 或静态断言无法编译 | 检查 smoke 是否私有链接 `chathub::contracts`、target 的 `cxx_std_20` 是否仍为 INTERFACE；不在测试 target 手工重复 `cxx_std_20` 掩盖合同传播问题 |
| CTest 找不到测试 | 检查顶层 `include(CTest)`、`BUILD_TESTING` 和 `add_test`；可执行文件存在不是测试已注册 |
| CTest 运行失败 | 保留失败输出，先区分二进制启动失败、非零退出与测试发现失败 |

## 7. 验收命令与通过标准

在本机环境变量／CLion User Preset 已配置的前提下，使用新的 Debug 构建目录：

```powershell
cmake --list-presets
ctest --list-presets
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
git diff --check
git status --short
```

通过标准：三个 configure／build／test presets 都可列出；Debug configure、build、CTest 均为退出码 0；CTest 报告 `1/1` 通过、`0` 失败、`0` 跳过；构建目录、vcpkg 安装目录和本机 User Preset 不进入 Git。

M0-3 不添加 Qt 或业务测试，因此没有人工 UI 验收。它也不代替后续协议、TLS、数据库或 E2E 测试。

## 8. 确认与实施入口

确认后，用户默认自行创建生产／构建配置；只有明确说“帮我实现 M0-3”时，AI 才代写本 PRP 所列文件。用户已授权 AI 在稳定接口后补测试，但该授权不扩大到业务架构、依赖或公共协议变更。
