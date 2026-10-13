# WorkAreaFullscreen

把游戏/应用「无边框全屏」的窗口改成「工作区全屏」：窗口铺满桌面工作区，保留任务栏等空间，
同时保持无边框（可继续用 Alt+Tab 切出）。

## 组成

| 产物 | 角色 |
| --- | --- |
| `WorkAreaFullscreen.exe` | 配置编辑器 + 启动代理管理者。**不监听进程、不注入** |
| `HookLauncher.exe` | 启动代理：以调试器身份启动目标，在配置的延迟之后注入，注入完成即脱离调试 |
| `injector.dll` | 被注入目标进程的 hook 模块，按配置改写 API 返回值 |

三者必须放在同一目录。

## 工作原理

以《Stellaris》（SDL 2.26.5 静态链接 + GDI）为例，无边框全屏窗口的几何来自两处：

```
窗口尺寸 <- GDI32!GetDeviceCaps(hdc, HORZRES/VERTRES)  -> SDL display.desktop_mode
         <- USER32!EnumDisplaySettingsW(device, i)      -> SDL display_modes 列表
窗口位置 <- USER32!GetMonitorInfoW(rcMonitor)           -> SDL_WINDOWPOS_CENTERED 居中
```

尺寸有两条来源，且都只在特定时机被读取：`desktop_mode` 由 `WIN_AddDisplay` 在 **SDL 视频初始化**
或收到 `WM_DISPLAYCHANGE` 时调用 `GetDeviceCaps` 填充；`display_modes` 列表由 `WIN_GetDisplayModes`
逐项调用 `EnumDisplaySettingsW` 填充。

因此 hook 模块做三件事：改 `GetDeviceCaps`、改 `EnumDisplaySettingsW`、注入后主动发一次
`WM_DISPLAYCHANGE` 让 SDL 重建 `desktop_mode` 缓存（由每行的 `refresh_displays` 选择，
默认不刷新）；
位置则用 `GetMonitorInfoW` 把 `rcMonitor`
换成 `rcWork`。三者一致后，居中结果正好等于 `rcWork`，窗口铺满工作区。

**关键前提是注入要早于目标的显示初始化**——但也不能太早：实测在目标尚未完成初始化时注入，
会让群星之类的引擎直接启动失败。所以每条规则带一个 `inject_delay_ms`，默认 400 ms，
既避开"太早崩溃"，也仍早于目标的显示初始化（外部轮询发现再注入最坏要到 695 ms 之后，
`GetDeviceCaps` 一次都没被调用）。

针对具体游戏的配置与原理见 `docs/`（当前：`docs/stellaris.md`）。

## 启动流程

```
任意程序启动 <目标>.exe
      │  Windows IFEO（HKLM\...\Image File Execution Options\<目标>.exe 的 Debugger 值）
      ▼
HookLauncher.exe
      │  以调试器身份创建真正的 <目标>.exe（DEBUG_ONLY_THIS_PROCESS）
      ▼
ContinueDebugEvent 立即放行，目标开始正常初始化
      │  等配置要求的 inject_delay_ms（默认 400 ms）
      ▼
注入 injector.dll（远程线程 LoadLibraryW）
      │  注入完成即解除调试关系（DebugActiveProcessStop）
      ▼
目标继续独立运行（HookLauncher 退出，不随目标结束）
```

`DEBUG_ONLY_THIS_PROCESS` 有两个作用：让调试器先接管启动（从而避免带 DEBUG 标志的
`CreateProcess` 再触发一次 IFEO 造成递归），以及能在目标跑起来之后仍由我们决定注入时机。
延迟由**独立计时线程**完成，目标在这段时间里已经正常跑完自己的初始化。

调试关系只在**注入期间**保持：注入线程一结束，主线程就调用 `DebugSetProcessKillOnExit(FALSE)`
和 `DebugActiveProcessStop`，目标不再带调试器运行，`HookLauncher` 随即退出。前者的默认值是
`TRUE`，若不关掉，调试器退出时会把目标一并杀掉。

## 快速开始

```powershell
# 1) 构建（需要 MSVC；第三方依赖见下）
cmake --preset release
cmake --build --preset release

# 2) 打开配置界面，编辑规则
.\build\release\WorkAreaFullscreen.exe

# 3) 挂载启动代理（界面里点「挂载」，或命令行；需要管理员权限）
.\build\release\HookLauncher.exe --install "<游戏目录>\stellaris.exe"

# 4) 之后从任意方式启动游戏（含 Steam）即可
```

摘掉启动代理、恢复原始启动行为：

```powershell
.\build\release\HookLauncher.exe --uninstall <目标.exe>
```

或用界面里每条规则的「摘除启动代理」按钮。

> **风险提示**：挂上 IFEO 后，任何对 `<目标>.exe` 的启动都会先经过 `HookLauncher.exe`。
> 因此务必保证 `HookLauncher.exe` 与 `injector.dll` 已就位、且与目标位数一致；否则目标会启动失败。
> 恢复方式就是上面的 `--uninstall`（或删除该键下的 `Debugger` 值）。

## 目录布局

```
CMakeLists.txt               构建脚本（MSVC x64）
CMakePresets.json            configure/build/test 预设
config/config.yaml           默认配置（安装包里的示例）
docs/                        每个游戏一份针对性说明（如 stellaris.md）
installer/                   Inno Setup 打包脚本
include/waf/                 公共头
  component.h                HkComponent 接口 + 静态注册宏
  config.h                   配置模型（Parse / Dump / FindRule）
  ifeo.h                     Image File Execution Options 读写
  runtime.h                  日志、策略调用点表（配置装载）、调用点判定、几何查询
  yamlmini.h                 最小 YAML 子集解析器（含序列化）
include/app/app.h            WorkAreaFullscreen 程序接口（状态、配置读写、启动代理管理）
src/core/                    waf_core：yamlmini / runtime / config / ifeo
src/app/                     WorkAreaFullscreen.exe（界面：配置编辑 + 启动代理管理）
src/launcher/                HookLauncher.exe（启动代理）
src/hook/                    injector.dll
  dllmain.cpp                读配置 -> 匹配进程 -> 安装组件 hook
  components/                一个组件一个文件
tests/                       yamlmini_test / probe_host / 启动代理集成自检
```

## 构建

需要 MSVC x64。第三方依赖均由 CMake `FetchContent` 自动拉取（首次 configure 需要网络）：

- **MinHook v1.3.3**：上游不含 CMakeLists.txt，本项目只取源码自行编译；需要指向本地已有源码时用
  `-DFETCHCONTENT_SOURCE_DIR_MINHOOK=<dir>`。
- **Dear ImGui v1.91.5**（Win32 + D3D11 后端）。

在 VS2022 Developer 环境（如 "x64 Native Tools Command Prompt for VS 2022"）中：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## 配置

```yaml
enabled: true              # 总开关（HookLauncher 注入时读）

targets:
  - executable: stellaris.exe   # 进程映像名，不区分大小写
    enabled: true
    strategy: sdl2-gdi          # 引用 strategies 里的调用点 RVA 表
    geometry: work_area         # work_area = rcWork；monitor = rcMonitor（不改写）
    inject_delay_ms: 400        # 目标启动后多少毫秒再注入（每条规则可单独设）
    refresh_displays: sdl       # 显示器缓存刷新方式：none（默认）| sdl
    hooks:
      GetMonitorInfoW:
        enabled: true
        fields:                 # dest: src，把 dest 字段复制为 src 字段
          rcMonitor: rcWork
        scope: callsite
      GetDeviceCaps:
        enabled: true
        indices:                # 只改写这些索引
          - HORZRES
          - VERTRES
        scope: callsite
      EnumDisplaySettingsW:
        enabled: true
        scope: global
    safety:
      verify_callsite: true     # 注入前校验调用点指令形态

# 调用点 RVA 表（相对目标映像基址）：绑定 stellaris.exe + 静态链接的 SDL 2.26.5
strategies:
  sdl2-gdi:
    GetMonitorInfoW:
      - 0x1DFAAF5
      - 0x1DFABA1
      - 0x1DFB491
      - 0x1DFE184
    GetDeviceCaps:
      - 0x1DFBB35
      - 0x1DFBB45
```

**`scope`**

- `callsite`：只对 `strategies` 里该策略记录的该组件调用点生效，其余调用原样放行（生产配置）。
- `global`：对该 API 的所有调用生效。

**`verify_callsite`** 按 `strategies` 的 RVA 检查该处是否为一条 `call qword ptr [rip+disp32]`；
版本变化导致 RVA 失配时直接回退为不改写，不会写坏进程。内存读取以 SEH 保护。

**`strategies`**（顶层）是调用点 RVA 表的定义处，结构为
`策略名 -> 组件名 -> 调用指令 RVA 列表`，RVA 相对目标映像基址（支持 `0x` 十六进制）。目标规则的
`strategy` 按名字引用其中一张表。表从配置文件装载，不再编译进代码：适配新版本时只改这里，
不需要重新编译 `injector.dll`。

界面有「规则」「RVA 配置」「IFEO 管理」三个页：「RVA 配置」页可直接增删策略与组件、编辑每一条
RVA（十六进制），保存后写回配置文件的 `strategies` 段。

**`refresh_displays`**（默认 `none`）选择注入后的显示器缓存刷新方式：

- `none`：不刷新。
- `sdl`：`hook` 模块等目标出现窗口后向其发一次 `WM_DISPLAYCHANGE`，让 SDL 重新枚举显示器并
  按改写后的尺寸重建 `desktop_mode`。目标是 SDL 时用这个；不做显示初始化的进程（如自检用的
  `probe_host.exe`）不必开启。

界面里每条规则都有「挂载 / 摘除启动代理」按钮，挂载状态直接读注册表，不写进配置文件。

**热重载**：外部编辑并保存配置文件后，程序在下次检查（约 1 s）时自动重载；界面里有未保存
修改时忽略外部版本。配置只在**下次注入**时生效，已经注入的进程需要重启目标。

## 扩展一个 hook 组件

1. 在 `src/hook/components/` 新建 `.cpp`，定义 detour 与 `configure` / `verify`；
2. 定义静态 `waf::HkComponent` 实例，`defaultConfig` 写该组件被新加入规则时的默认参数；
3. 用 `HK_REGISTER_COMPONENT` 注册；
4. 在 `CMakeLists.txt` 的 `injector` 目标里加上该源文件。

宿主遍历注册表读配置，不需要改动。界面里的「新增组件」列表来自 `injector.dll` 导出的组件目录
（`HkComponentCount` / `HkComponentName` / `HkComponentDefaultConfig`），新组件写好
`defaultConfig` 后会自动出现在列表里。

## 适配新的目标版本

`strategies` 里的 RVA 绑定具体版本的 exe。游戏更新导致调用点移位时：

1. 重新定位调用点（方法与源码对应关系见 `docs/stellaris.md`）；
2. 编辑对应的 RVA 列表：界面「RVA 配置」页，或直接改配置文件的 `strategies` 段；
3. 无需重新编译：`injector.dll` 下次注入时按新表工作。

`scope: callsite` 的组件若在表里找不到该组件的调用点、或 `verify_callsite` 校验失败，会整条
回退为不改写，只影响该组件本身。

## 自检

```powershell
ctest --preset release                       # YAML/配置的解析与序列化往返
pwsh -File tests\run_launcher_test.ps1       # 用启动代理启动 probe_host 并验证注入与改写
```

`run_launcher_test.ps1` 走的就是生产路径：`HookLauncher` 启动目标 → 断言注入时的目标运行时间
与配置的 `inject_delay_ms` 相符（probe.yaml 配的是 0）→ 断言 `rcMonitor` 被改写为 `rcWork`、
且 SDL 居中结果等于工作区。

## 打包

```powershell
cmake --preset release; cmake --build --preset release
ISCC.exe installer\WorkAreaFullscreen.iss
```

产物为 `dist\WorkAreaFullscreen-Setup-<版本>.exe`。安装包内含三个组件与示例配置；可选任务包括
开机自启与桌面快捷方式。启动代理（IFEO）不随安装挂载，安装后在界面里按目标按需挂载。
用户配置存放在 `%APPDATA%`，卸载时保留。
