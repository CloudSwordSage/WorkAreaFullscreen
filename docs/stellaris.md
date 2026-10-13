# 群星（Stellaris）

把游戏的「无边框全屏」窗口改成「工作区全屏」：铺满桌面工作区（保留任务栏等空间），
窗口保持无边框，仍可用 `Alt+Tab` 切出。

| 项 | 值 |
| --- | --- |
| 进程名 | `stellaris.exe`（不区分大小写） |
| 引擎 | Clausewitz |
| 窗口层 | SDL 2.26.5（静态链接） |
| 渲染 | D3D11 / DXGI |
| 策略 | `sdl2-gdi` |
| 默认注入延迟 | 400 ms |

## 配置

```yaml
enabled: true

targets:
  - executable: stellaris.exe
    enabled: true
    strategy: sdl2-gdi
    geometry: work_area
    inject_delay_ms: 400
    refresh_displays: sdl
    hooks:
      GetMonitorInfoW:
        enabled: true
        fields:
          rcMonitor: rcWork
        scope: callsite
      GetDeviceCaps:
        enabled: true
        indices:
          - HORZRES
          - VERTRES
        scope: callsite
      EnumDisplaySettingsW:
        enabled: true
        scope: global
    safety:
      verify_callsite: true
```

挂载启动代理（需要管理员权限；界面里点「挂载」等价）：

```powershell
HookLauncher.exe --install "<游戏目录>\stellaris.exe"
```

## 为什么是这三个组件

群星的无边框全屏窗口是引擎在启动时以 `WS_POPUP` 样式创建的，**创建之后不再切换样式**，
所以窗口的无边框无需处理。需要改的是创建时的尺寸与位置，二者的来源不同：

```
窗口尺寸 <- GDI32!GetDeviceCaps(hdc, HORZRES/VERTRES) -> SDL display.desktop_mode / current_mode
         <- USER32!EnumDisplaySettingsW(device, i)     -> SDL display_modes 列表
窗口位置 <- USER32!GetMonitorInfoW(rcMonitor)          -> SDL_WINDOWPOS_CENTERED 居中
```

- **尺寸**：SDL 的 `WIN_AddDisplay` 在视频初始化时用 `GetDeviceCaps` 填充
  `desktop_mode`；`display_modes` 列表由 `WIN_GetDisplayModes` 逐项调用
  `EnumDisplaySettingsW` 填充。引擎取到哪个来源是版本相关的，两条路径都要覆盖，
  所以同时改写 `GetDeviceCaps(HORZRES/VERTRES)` 与 `EnumDisplaySettingsW` 的当前模式。
- **位置**：引擎传入 `SDL_WINDOWPOS_CENTERED`，SDL 用显示器矩形居中。把
  `GetMonitorInfoW` 的 `rcMonitor` 换成 `rcWork` 后，居中结果正好落在工作区。

三者一致后，窗口的尺寸等于工作区尺寸、并居中于工作区，即铺满工作区。

> 配置文件的 `settings.txt` / `pdx_settings.txt` 以及 `GetSystemMetrics`、`EnumDisplaySettingsW`
> 的 `ENUM_CURRENT_SETTINGS` 项都**不是**该窗口几何的来源，不需要改。

## 注入时机

注入必须**早于游戏的显示初始化**（`GetDeviceCaps` / `EnumDisplaySettingsW` / `GetMonitorInfoW`
第一次被调用之前），但也不能太早——目标尚未完成初始化时注入会让引擎直接启动失败。

`inject_delay_ms` 默认 400 ms，能同时避开这两种失败。该值按规则单独配置：目标机器越慢，
显示初始化越晚，可适当调大；如果游戏在注入前就已完成显示初始化（日志里三个组件都没有命中），
则需要调小。

`refresh_displays`（默认 `none`）选择注入后的显示器缓存刷新方式：设成 `sdl` 时，`hook` 模块
等游戏出现 SDL 窗口后发一次 `WM_DISPLAYCHANGE`，让 SDL 按改写后的尺寸重新枚举显示器、重建
`desktop_mode`。群星需要这一项，配置里显式写成 `sdl`。

## 策略与生效范围

`strategy: sdl2-gdi` 提供与该版本 `stellaris.exe` 绑定的调用点 RVA 表：

| 组件 | 调用点（相对 ImageBase） |
| --- | --- |
| `GetMonitorInfoW` | `0x1DFAAF5`、`0x1DFABA1`、`0x1DFB491`、`0x1DFE184` |
| `GetDeviceCaps` | `0x1DFBB35`、`0x1DFBB45` |

`scope: callsite` 只对表中的调用点生效，游戏或系统其它地方对同一 API 的调用原样放行；
`EnumDisplaySettingsW` 的调用点不在表中，只能按 `scope: global` 处理。

`safety.verify_callsite: true` 会在注入前按 RVA 核对指令形态（`call qword ptr [rip+disp32]`），
任一调用点对不上就整条组件回退为不改写。这样游戏更新换了版本、RVA 失配时，后果只是「功能不生效」，
不会写坏进程。

## 已知限制

- RVA 表绑定特定版本的 `stellaris.exe` + 静态链接的 SDL 2.26.5。游戏大版本更新后需要重新定位
  调用点并更新策略表。
- 改写只覆盖主窗口的创建路径；启动阶段的 logo 小窗口（同为 `SDL_app` 类）不在覆盖范围内。
- 已经启动的游戏进程不受影响，改动在下次注入时生效，需要重启游戏。

## 如何确认生效

注入后看 hook 日志 `%TEMP%\WorkAreaFullscreen-hook.log`，关键行形如：

```
GetMonitorInfoW(mon=...) rcMonitor=(...) -> rcWork=(...)
GetDeviceCaps(HORZRES) hdc=... <原值> -> <工作区宽>
EnumDisplaySettingsW(\\.\DISPLAY1 mode=0) <原尺寸> -> <工作区尺寸>
```

三条都出现且数值等于工作区尺寸，即为生效。若日志里出现「调用点不在策略表中」或
`callsite 0x... mismatch`，说明版本与策略表不匹配（见上）。

项目自带的自检不走真实游戏，而是用 `probe_host.exe` 走同一条注入路径：

```powershell
ctest --preset release
pwsh -File tests\run_launcher_test.ps1
```
