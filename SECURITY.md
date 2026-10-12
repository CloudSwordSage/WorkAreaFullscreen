# 安全政策

## 支持的版本

安全修复只针对最新发布版本（`v*` tag）与 `main` 分支。旧版本请先升级到最新版本后再报告问题。

## 报告漏洞

请**不要**通过公开 Issue 报告安全漏洞。

优先使用 GitHub 的私有漏洞报告：进入本仓库的 **Security → Report a vulnerability**，
在仅维护者可见的草稿中提交内容。若该入口不可用，请通过仓库维护者页面上的联系方式私下沟通，
并在得到确认之前不要公开细节。

报告请尽量包含：

- 受影响的版本或提交（`git rev-parse HEAD`）与构建方式；
- 最小化的复现步骤；
- 实际行为与预期行为；
- 相关日志（见下方「诊断信息」）；
- 可能的影响范围，以及是否已验证可被利用。

## 诊断信息

- 启动代理：`%TEMP%\WorkAreaFullscreen-launcher.log`
- 注入模块：`%TEMP%\WorkAreaFullscreen-hook.log`
- 用户配置：`%APPDATA%\WorkAreaFullscreen\config.yaml`

日志可能包含本机路径与目标进程名，公开前请自行脱敏。

## 设计边界与滥用风险

以下系统操作是本项目的功能，属于**预期行为**，不是漏洞：

- 通过 Image File Execution Options（`HKLM\...\Image File Execution Options`）挂载启动代理，需要管理员权限；
- 以调试器身份启动目标进程（`DEBUG_ONLY_THIS_PROCESS`）；
- 向目标进程注入 `injector.dll` 并挂钩 Win32 API。

以下情况视为安全漏洞，欢迎按上述渠道报告：

- 无需管理员权限即可写入 IFEO 或完成注入；
- 加载未校验路径的 DLL（例如可被同目录同名文件劫持）；
- 超出上述预期行为的提权、任意代码执行或持久化；
- 配置解析导致越界、崩溃或内存破坏。

## 响应

维护者会尽力在收到报告后确认并评估，修复随下一个版本发布。本项目为个人维护，不承诺固定的响应时限。
