# M0-4 PRP：Git 提交范围收口

> 状态：已交付（2026-09-01）。用户授权 AI 执行提交并推送 `origin/main`；学习笔记最终定于 `docs/学习笔记/` 且由用户加入 `.gitignore`，不随本步入库。

## 1. 目标与范围

把当前工作区剩余未提交改动按主题入库：M0-3 构建与测试产物、`.clang-format`、实时状态与周计划文档。完成后 `git status --short` 无未预期条目，W0 Git 边界收口。

不做：push／tag／分支；修改 `.gitignore` 语义；纳入学习路线；修改任何生产／构建文件。

## 2. 状态与授权

| 当前状态 | 已给出待落地 |
| --- | --- |
| Git | 允许暂存＋提交（用户执行）；禁止 push／tag |
| 生产代码 | 无变更 |

## 3. 追踪

| 来源 | 条目 |
| --- | --- |
| 路线 | W0 阶段验收“新仓库初始化后只纳入允许文件”；提交前缀表 |
| 设计 | D175—D185 |

## 4. 上下文

- `git log`：`74533c2` 已入库 M0-1—M0-3 骨架（AGENTS、命令、PRP 模板、上下文工程、周计划、shared、vcpkg、CLAUDE.md、合同文档修改、忽略规则）；
- 用户指令（2026-09-01）：`TASK.md` 与 `INITIAL.md` 的功能并入当周需求与验收文档——两文件已删除，`docs/周计划/W0-需求与验收.md` 增设“实时状态”节成为仓库内唯一实时状态源；学习笔记定于 `docs/学习笔记/`（按技术栈分文件，随本步入库）；
- 当前未提交：`tests/`（已暂存）、根 `CMakeLists.txt`、`CMakePresets.json`（testPresets）、`.clang-format`（已暂存）、AGENTS／索引／命令／模板／周计划更新、`TASK.md`／`INITIAL.md` 删除；
- `.gitignore` 已生效：`out/`、`vcpkg_installed/`、`CMakeUserPresets.json`、路线文档等。

## 5. 实施蓝图（同一时间只执行一个 Task）

```yaml
Task 1 构建与测试基线（chore）:
  add: [.clang-format, tests/, CMakeLists.txt, CMakePresets.json]
  commit: "chore: CTest smoke 闭环、test presets 与 clang-format 规则（M0-3）"

Task 2 实时状态并入周计划（docs）:
  add: [AGENTS.md, .gitignore, docs/上下文工程/上下文索引.md, .agent/commands/,
        PRPs/templates/capability_prp.md,
        PRPs/M0-3-最小CTest-smoke闭环与测试Preset.md,
        PRPs/M0-4-Git提交范围收口.md, PRPs/M1-1-客户端与服务端分层CMake骨架.md,
        docs/周计划/, docs/项目规划/详细设计与技术选型.md, TASK.md, INITIAL.md]
  note: TASK.md 与 INITIAL.md 已删除；git add 按路径暂存其删除；docs/学习笔记 被 .gitignore 忽略，不暂存
  commit: "docs: 实时状态并入周计划，移除 TASK 与 INITIAL"

Task 3 验证（只读）:
  commands: [git status --short, git log --oneline -3, git diff --check,
             git ls-files]
  done_when: status 无未预期条目；ls-files 无构建产物／vcpkg_installed／日志／秘密
```

## 6. 风险与停止条件

- `git status` 出现未预期文件或路径：停止并确认；禁止 `git add .`；
- LF→CRLF 警告来自 core.autocrlf，属正常，不阻断；如需固定行尾，另立 `.gitattributes` 小步；
- 合同文档（产品方案／详细设计）已随 `74533c2` 入库，本步不再触碰。

## 7. 完成清单

- [ ] 两次提交完成且信息与前缀一致；
- [ ] `git status --short` 无未预期条目；
- [ ] `git ls-files` 抽查通过；
- [ ] `docs/周计划/W0-功能代码对照.md` M0-4 行更新为已交付。
