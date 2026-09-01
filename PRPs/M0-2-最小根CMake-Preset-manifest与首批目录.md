# M0-2 PRP：最小根 CMake、Preset／manifest 与首批目录

> 状态：已交付（2026-09-01）。用户已明确授权“帮我创建”；AI 仅实施本 PRP 的构建配置范围。

> 用户输入：用户已确认根 `vcpkg.json` 是其草稿。该文件保持用户所有权；用户已将 `name` 修正为 `chathub` 并将 manifest baseline 对齐到 `30ef65cad98f08e7197c9a1656fbd871bcb72f2d`。本 PRP 未覆盖、暂存或删除该文件。

## 1. 目标与边界

在 M0-1 已确认的目录／target 职责上，建立一个**可配置但尚不包含业务功能**的 C++20 根工程。它只创建 `shared/` 的 `chathub_contracts` 空接口 target；`client-qt/`、`chat-server/`、协议、业务源码和测试留给后续已确认能力点。

本步落实 D64、D65、D177、D183、D208，不改产品、协议、数据库或 UI 合同。

## 2. 新鲜环境事实

| 输入 | 已验证结果 | 用途 |
| --- | --- | --- |
| CMake | 4.3.2 | 可读取 Presets；项目最低版本拟定为 3.23 |
| Ninja | 1.13.2 | 唯一生成器 |
| MinGW | `D:\QT\Tools\mingw1310_64`，GCC 13.1.0 | `c++.exe` 与运行时必须进入 Preset 环境 |
| Qt | `D:\QT\6.11.1\mingw_64`，`Qt6Config.cmake` 存在 | 作为未来 Qt 适配层的受控输入；本步不调用 `find_package(Qt6)` |
| CLion vcpkg | `C:\Users\Administrator\.vcpkg-clion\vcpkg`，`vcpkg.exe`／`vcpkg.cmake` 存在，HEAD `30ef65…` | 本机 `VCPKG_ROOT`；不克隆、不 bootstrap、不修改该 IDE 管理目录 |

CLion vcpkg 当前 HEAD `30ef65cad98f08e7197c9a1656fbd871bcb72f2d` 是本项目本步采用的固定快照。用户须将其写入 `builtin-baseline`；该值不在执行中自动更新。

## 3. 确认后的最小文件与目录

| 路径 | 内容／职责 |
| --- | --- |
| `CMakeLists.txt` | `cmake_minimum_required(VERSION 3.23)`、`project(ChatHub VERSION 1.0.0 LANGUAGES CXX)`、`CMAKE_CXX_EXTENSIONS OFF`、`include(CTest)`、`add_subdirectory(shared)` |
| `CMakePresets.json` | schema 版本 4；`windows-mingw-debug`、`windows-mingw-relwithdebinfo`、`windows-mingw-release` 的 configure／build presets；统一使用 Ninja 与 `out/build/<preset>` |
| `vcpkg.json`（用户草稿） | 保留 `name=chathub`、`version-string=1.0.0` 和空 `dependencies` 数组；用户把 `builtin-baseline` 更新为 `30ef65…`；后续依赖在其首次使用的能力点追加 |
| `shared/CMakeLists.txt` | `chathub_contracts` 的 `INTERFACE` target、`chathub::contracts` 别名、`cxx_std_20` 公共编译特性；不含 `.cpp`／`.h` 占位文件 |
| `.gitignore` | 增加 `vcpkg_installed/`，继续忽略 `out/`、`CMakeUserPresets.json`、数据库、日志与秘密 |

不创建其余 D183 目录；只有在相应能力点拥有实际源码或已确认构建职责时才创建。

## 4. Preset 数据流与字段合同

```text
本机环境变量 → CMakePresets.json → CMake cache/toolchain
→ vcpkg manifest + builtin-baseline → out/build/<preset>
```

Preset 只读取以下三个机器专属输入，且不写入 Git：

| 变量 | 当前建议值 | 使用位置 |
| --- | --- | --- |
| `QT_ROOT_DIR` | `D:\QT\6.11.1\mingw_64` | `CMAKE_PREFIX_PATH` |
| `CHATHUB_MINGW_ROOT` | `D:\QT\Tools\mingw1310_64` | `CMAKE_CXX_COMPILER` 和 `PATH` |
| `VCPKG_ROOT` | `C:\Users\Administrator\.vcpkg-clion\vcpkg` | `toolchainFile`；由 CLion CMake Profile 的本机环境配置 |

- `CMakePresets.json` 可进入 Git；`CMakeUserPresets.json` 只能保存用户机专属覆盖，保持忽略。
- `binaryDir` 固定为 `${sourceDir}/out/build/${presetName}`，每个 preset 独立，禁止复用旧 build cache。
- `VCPKG_TARGET_TRIPLET` 与 `VCPKG_HOST_TRIPLET` 都是 `x64-mingw-dynamic`。
- 空 manifest 只固定项目级依赖解析入口和 baseline；Asio、OpenSSL、Boost.JSON、libsodium、libmariadb 等在首次真实使用时再经 PRP 加入，避免提前下载未使用依赖。

## 5. 实施结果

1. 用户草稿中的 `builtin-baseline` 已对齐 `30ef65cad98f08e7197c9a1656fbd871bcb72f2d`；未修改 CLion vcpkg 目录。
2. 已创建第 3 节的其余文件和 `shared/`；未创建客户端／服务端／协议占位代码。
3. 在当前验证进程设置 `QT_ROOT_DIR`、`CHATHUB_MINGW_ROOT`、`VCPKG_ROOT` 后完成构建；这些本机路径没有写入受控文件。
4. 新鲜 `out/build/windows-mingw-debug` configure 与 build 成功。M0-2 没有测试 target，故未运行 CTest；M0-3 才添加 smoke test 和 test preset。
5. 已检查 `git diff --check`、`.gitignore`、vcpkg baseline 与 Git 状态；构建和依赖产物均被忽略。

## 6. 失败路径

| 现象 | 处理 |
| --- | --- |
| `VCPKG_ROOT` 为空或 toolchain 不存在 | 停止 configure；核对 CLion CMake Profile 的本机环境和 `C:\Users\Administrator\.vcpkg-clion\vcpkg\scripts\buildsystems\vcpkg.cmake`，不安装第二套 vcpkg |
| CMake 选错编译器或找不到 `ld.exe` | 核对 `CHATHUB_MINGW_ROOT`、`CMAKE_CXX_COMPILER` 和 Preset 的 `PATH`，删除当前 `out/build/<preset>` 后重新配置 |
| 找不到 Qt 包 | 核对 `QT_ROOT_DIR/lib/cmake/Qt6/Qt6Config.cmake` 与 MinGW 13.1 架构；不混用 MSVC／MSYS2 Qt |
| baseline 与 CLion vcpkg HEAD 不同 | 停止，更新项目 manifest 的 baseline；不得修改 IDE 管理的 vcpkg checkout 或静默选用另一套工具 |
| `vcpkg_installed/` 出现在 Git 状态 | 先修正 `.gitignore`，再继续构建 |

## 7. 验收命令与通过标准

确认后由用户执行（路径按其实际 `VCPKG_ROOT` 设置）：

```powershell
cmake --list-presets
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
git diff --check
git status --short
```

通过标准：三个 preset 可列出；Debug 能从空 `out/build/windows-mingw-debug` 完成 configure 和 build；CMake 缓存显示 C++20、Ninja、MinGW 和 vcpkg toolchain；不出现未忽略的构建／依赖产物。无 CTest 不是通过或跳过，而是本步明确不适用。

## 8. 确认与实施入口

用户已说明：受控 `CMakePresets.json` 统一 Ninja、编译器、构建目录与 triplet；`CMakeUserPresets.json` 仅保存本机路径；vcpkg checkout 必须匹配固定 baseline；当前只建立 shared 公共合同边界。PRP 据此确认。

实际执行结果：`cmake --list-presets` 列出三个 configure presets；`cmake --preset windows-mingw-debug` 使用 Ninja、MinGW GCC 13.1.0 与 CLion vcpkg toolchain 配置成功；`cmake --build --preset windows-mingw-debug` 成功并报告 `ninja: no work to do.`。无 CTest 不计为通过或跳过，而是本步明确不适用。
