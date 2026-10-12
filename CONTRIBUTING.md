# 贡献指南

## 环境

- Windows 11 x64，MSVC（VS2022）+ Ninja + CMake ≥ 3.20
- **MinHook v1.3.3** 与 **Dear ImGui v1.91.5**：均由 CMake `FetchContent` 自动拉取（首次 configure 需要网络）。
  需要指向本地已有的 MinHook 源码时用 `-DFETCHCONTENT_SOURCE_DIR_MINHOOK=<dir>`。

## 构建与自检

在 VS2022 Developer 环境（"x64 Native Tools Command Prompt for VS 2022"）中：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
pwsh -File tests\run_launcher_test.ps1
```

提交前请确保构建与自检均通过。

## 分支与提交

- 所有改动基于当前开发分支，**不直接向 `main` 提交**。
- 提交格式：`<类型>(<范围>): <动词开头描述>`，例如 `fix(hook): 修正调用点校验的返回地址获取`。
- 类型：`feat` / `fix` / `refactor` / `docs` / `test` / `chore`。
- 每个提交必须可独立回滚；`feat` 与 `refactor` 不混合提交。
- 不写无信息量的描述（`update`、`fix bug` 等）。

## 代码约定

- C++17，Windows x64 ABI，一律静态 CRT（`MultiThreaded`）。
- 编译期统一 `WIN32_LEAN_AND_MEAN`、`NOMINMAX`，并保持 `/W3 /permissive-` 无新增警告。
- 注释说明「为什么」，不复述代码；业务规则、隐式状态机、副作用、并发与生命周期边界必须注释。
- 公共接口使用 Doxygen 风格文档注释（`@param` / `@return` / `@throws`），与现有头文件保持一致。
- 优先复用已有实现与标准库；引入新的第三方依赖前先与维护者讨论。

## 目录

- `include/waf/`：`waf_core` 公共头（配置模型、YAML、运行环境、IFEO）。
- `include/app/`：`WorkAreaFullscreen.exe` 的程序接口。
- `src/core/`：`waf_core` 实现。
- `src/app/`：配置编辑器（`WorkAreaFullscreen.exe`）。
- `src/launcher/`：启动代理（`HookLauncher.exe`）。
- `src/hook/`：注入模块（`injector.dll`）与 hook 组件，一个组件一个文件。
- `tests/`：单元自检与启动代理集成自检。

新增 hook 组件的步骤见 README 的「扩展一个 hook 组件」。

## 报告问题

- Bug、功能请求与建议请使用仓库的 Issue 模板。
- 安全漏洞不要开公开 Issue，见 [`SECURITY.md`](SECURITY.md)。
