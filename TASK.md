# 当前任务状态

> 更新时间：2026-09-01  
> 同一时间只允许一个进行中能力点。

## 当前状态

- 旧 ChatHub 的复制、迁移参考、副本和旧构建配置已取消，不属于新项目。
- 当前仓库保留产品／设计合同、协作骨架和 M0-2 的最小构建配置；没有生产代码、业务依赖或测试二进制。新鲜构建目录存在但被 `out/` 忽略。
- M0-1 的规划 PRP 已由用户确认；目录职责、target 边界和 Git 忽略边界已完成文档级验收。
- M0-2 已交付：根 `CMakeLists.txt`、受控 `CMakePresets.json`、`shared/chathub_contracts` 接口 target 与 `vcpkg_installed/` 忽略规则均已创建；未创建客户端、服务端、协议或业务占位代码。
- 用户维护的 `vcpkg.json` baseline 已对齐 CLion 管理的 vcpkg HEAD `30ef65cad98f08e7197c9a1656fbd871bcb72f2d`；不修改该 IDE 管理目录。
- 新鲜 `windows-mingw-debug` configure／build 已通过；当前没有测试 target，因此本步未运行 CTest。
- 用户已报告 CLion 的本机 CMake 配置完成；本轮未重复执行 IDE 内 configure／build。
- M0-3 已进入仅规划状态：建立最小 CTest smoke 闭环与 test presets，不引入业务或第三方测试框架。
- 本周周计划文档已建立：`docs/周计划/W0-需求与验收.md` 与 `docs/周计划/W0-功能代码对照.md`，每次交付后同步更新对照文档。
- 协作模式已更新（AGENTS.md 第 4 节）：常规＝AI 给出实现与修改内容、用户落地；教学／检查模式仅在用户明确要求时进入。

## 下一候选能力点

- 编号：M0-3
- 名称：最小 CTest smoke 闭环与 test presets
- 状态：仅规划，PRP 已生成；尚未创建任何测试文件或修改构建配置

## 当前范围

- 保持 `D:\CppLearn\chathub` 不动且不复制；
- 从 `D:\全栈聊天软件` 的空白工程根目录开始；
- M0-1 已确认的目录职责和 target 边界已落实为 M0-2 的最小构建入口；
- M0-3 只新增 `tests/` 的实际测试职责、一个消费者 smoke target 与 CTest 注册；
- 保持 `shared` 纯 C++20，不创建客户端、服务端、协议或业务模型。

## 非目标

- 恢复旧 ChatHub 源码、Node.js Auth Service、旧协议或旧构建配置；
- 修改产品需求、协议、数据库或 UI 行为；
- 安装或升级依赖、引入 GoogleTest／Catch2 或写测试框架封装；
- 进入客户端／服务端、协议、数据库或 M1 工作。

## 下一触发

1. 用户说“下一步”→ 直接给出 M0-3 的实现与修改内容（`tests/` smoke target、CTest 注册、test presets），由用户落地；只有用户明确要求“帮我实现／修改／修复 M0-3”时，AI 才直接创建测试源码并修改 CMake／Preset；
2. 用户说“检查”→ 按验证矩阵对已完成内容（当前为 M0-2 构建配置）执行分层验证；
3. 不提前进入 M0-4、M1 或任何业务功能。
