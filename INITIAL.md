# 当前候选能力点需求输入

> 本文件描述 M0-3 候选；用户说“下一步”即进入其实现内容交付。只有用户明确要求“帮我实现／修改／修复”时，AI 才直接修改 CMake、Preset 或创建测试源码。

## 能力点

M0-3：最小 CTest smoke 闭环与 test presets。

## 目标

让当前空的 `chathub::contracts` 公共合同 target 第一次被真实消费者编译、执行和由 CTest 发现。该闭环验证构建配置，而不是宣称已有产品功能或完整测试体系。

## 当前上下文

- M0-2 已提供 `include(CTest)`、Ninja configure／build presets 和 `chathub::contracts` 的 C++20 编译特性；但尚未注册任何 test。
- CTest 由顶层 `include(CTest)` 在 `BUILD_TESTING=ON` 时启用；`add_test(NAME … COMMAND …)` 才把可执行程序注册为可发现测试。
- CMake 3.23 的 schema v4 支持 `testPresets`、失败输出和“没有测试即错误”的执行策略。

## 本步范围

- 仅按 `tests/` 的实际测试职责创建一个 C++20 smoke 可执行 target 与 CTest 注册；
- 为 Debug、RelWithDebInfo、Release 增加同名 test presets；
- smoke 程序以 C++20 `<concepts>` 静态断言验证 `chathub::contracts` 的编译特性传播，并以退出码 0 表示启动成功；
- 从干净 Debug 构建目录完成 configure、build、CTest，并明确报告通过／失败／跳过数。

## 非目标

- 业务模型、客户端／服务端、Qt、网络、数据库、协议或产品测试；
- GoogleTest、Catch2、FetchContent、vcpkg 新依赖或自制测试框架；
- M0-4 Git 提交／推送或任何生产功能。

## 完成条件

- [ ] 用户已说“下一步”，并收到 M0-3 的实现与修改内容；
- [ ] `ctest --preset windows-mingw-debug` 能发现并运行恰好一个 smoke 测试；
- [ ] 无测试时 test preset 以错误退出，防止“0 tests”假绿；
- [ ] `tests/` 是唯一新增目录，且实际包含 target 和 CMakeLists；
- [ ] 交付后同步更新 `docs/周计划/W0-功能代码对照.md`；
- [ ] 实时状态只记录在 `TASK.md`、`INITIAL.md` 与 M0-3 PRP；学习笔记不自动记录进度。
