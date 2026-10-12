# WorkAreaFullscreen

把游戏/应用「无边框全屏」的窗口改成「工作区全屏」：窗口铺满桌面工作区，保留任务栏、
Dock 等系统占用的空间，同时保持无边框（可继续用 Alt+Tab 切出）。

常驻后台，带 ImGui 界面与托盘图标：在界面里配置多个应用的 hook 规则，程序监听这些
进程的启动并按规则注入；配置文件热重载，改完即对后续启动的进程生效。

## 原理

以《Stellaris》（SDL 2.26.5 静态链接 + GDI）为例，无边框全屏窗口的几何来自两处：

```
窗口尺寸 <- GDI32!GetDeviceCaps(hdc, HORZRES/VERTRES)  -> SDL display.desktop_mode
         <- USER32!EnumDisplaySettingsW(device, i)      -> SDL display_modes 列表（引擎可能取列表首项）
窗口位置 <- USER32!GetMonitorInfoW(rcMonitor)           -> SDL_WINDOWPOS_CENTERED 居中
```

尺寸有两条来源，且都只在特定时机被读取：

- `display.desktop_mode` 由 `WIN_AddDisplay` 在 **SDL 视频初始化** 或收到 **WM_DISPLAYCHANGE** 时调用
  `GetDeviceCaps` 填充，缓存后窗口创建只读缓存；
- `display_modes` 列表由 `WIN_GetDisplayModes` 逐项调用 `EnumDisplaySettingsW` 填充，**不经过**
  `GetDeviceCaps`。

所以框架做三件事：改 `GetDeviceCaps`、改 `EnumDisplaySettingsW`、并在注入后主动发一次
`WM_DISPLAYCHANGE` 让 SDL 重建 `desktop_mode` 缓存；位置则用 `GetMonitorInfoW` 把 `rcMonitor`
换成 `rcWork`。三者一致后，居中结果正好等于 `rcWork`，窗口铺满工作区。
完整定位过程与证据见 `stellaris_borderless_fullscreen_trace.md`（不在版本库内）。

## 组成

| 产物 | 角色 |
| --- | --- |
| `WorkAreaFullscreen.exe` | 常驻宿主：ImGui 界面、托盘、进程监听、注入、配置热重载 |
| `injector.dll` | 被注入目标进程的 hook 模块，按配置安装组件 |

两者共用一份配置：宿主默认读写 `%APPDATA%\WorkAreaFullscreen\config.yaml`，
DLL 按 `HOOKKIT_CONFIG` → DLL 同目录 → `%APPDATA%\WorkAreaFullscreen` → `%TEMP%` 查找。

## 目录布局

```
CMakeLists.txt               构建脚本（MSVC x64）
CMakePresets.json            configure/build/test 预设
config/config.yaml           默认配置（安装包里的示例）
installer/                   Inno Setup 打包脚本
include/waf/                 公共头
  component.h                HkComponent 接口 + 静态注册宏
  config.h                   配置模型（Parse / Dump / FindRule）
  runtime.h                  日志、策略调用点表、调用点判定、几何查询
  yamlmini.h                 最小 YAML 子集解析器（含序列化）
src/core/                    waf_core 静态库：yamlmini / runtime / config
src/hook/                    injector.dll
  dllmain.cpp                读配置 -> 匹配进程 -> 安装组件 hook
  components/                一个组件一个文件
    get_monitor_info.cpp     位置：rcMonitor -> rcWork
    get_device_caps.cpp      尺寸来源一：desktop_mode
    enum_display_settings.cpp 尺寸来源二：显示模式列表
src/app/                     WorkAreaFullscreen.exe
  main.cpp                   入口与命令行
  app.cpp                    窗口 + D3D11 + ImGui + 托盘 + 配置读写/热重载
  ui.cpp                     ImGui 界面
  watch.cpp                  进程扫描、延迟注入、单次注入模式
tests/                       yamlmini_test / probe_host / 集成自检脚本
```

## 构建

需要 MSVC x64。第三方依赖：

- **MinHook**：本地源码。查找顺序 `-DHOOKKIT_MINHOOK_DIR=<dir>` → `third_party/minhook`
  → `_mh/minhook-1.3.3`。
- **Dear ImGui v1.91.5**：由 CMake `FetchContent` 自动拉取（需要网络）。

在 VS Developer 环境（本仓库用 `VSDevShell { ... }`）中：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## 运行

```powershell
# 常驻（无参数）：界面 + 托盘，按配置监听并注入
.\build\release\WorkAreaFullscreen.exe

# 单次：按配置启动某个程序并注入，等它退出
.\build\release\WorkAreaFullscreen.exe --config config\config.yaml --inject <game.exe>
```

| 开关 | 说明 |
| --- | --- |
| `--config <path>` | 指定配置文件，默认 `%APPDATA%\WorkAreaFullscreen\config.yaml` |
| `--inject <exe>` | 单次注入模式 |
| `--delay <ms>` | 覆盖配置里的注入延迟 |

界面左上角是「保存 / 重新加载 / 收进托盘」，左侧编辑全局设置与规则，右侧是已注入进程与日志。
关闭窗口默认收进托盘（可在配置或界面里关掉）。托盘右键：显示主窗口 / 重新加载配置 / 退出。
宿主日志写到 `%TEMP%\WorkAreaFullscreen.log`，被注入模块的日志写到
`%TEMP%\WorkAreaFullscreen-hook.log`（两者分开，便于同时排查）。

**注入时机**：目标启动瞬间注入会让部分引擎崩溃，因此默认在其启动后 300 ms 注入
（`inject_delay_ms` 可调）。

**权限**：若目标以管理员身份运行，本程序也需以管理员身份启动，否则 `OpenProcess` 会被拒绝。

## 配置

```yaml
enabled: true              # 全局开关
inject_delay_ms: 300       # 目标进程“启动后”多少毫秒注入
scan_interval_ms: 200      # 进程扫描间隔

ui:
  minimize_to_tray: true   # 关闭窗口时收进托盘

targets:
  - executable: stellaris.exe   # 进程映像名，不区分大小写
    enabled: true
    strategy: sdl2-gdi          # 提供与目标版本绑定的调用点 RVA 表
    geometry: work_area         # work_area = rcWork；monitor = rcMonitor（不改写）
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
        scope: global           # 策略表里没有该组件的调用点，只能按全局处理
    safety:
      verify_callsite: true     # 注入前校验调用点字节与 IAT 槽
```

**注入时机**：`inject_delay_ms` 是**目标进程启动后**多少毫秒注入，不是"发现后"再等多久。
目标的显示初始化（SDL 枚举显示器、缓存 desktop_mode）只发生一次，注入晚于它就只能靠
`EnumDisplaySettingsW` + `WM_DISPLAYCHANGE` 补救。hook 日志里会记录注入时目标已运行多少毫秒，
便于判断是否赶上窗口期。

**目标发现**：默认按 `scan_interval_ms` 轮询进程列表，本机实测 200 ms 间隔的发现延迟约 120 ms。
另可接入 WMI 的 `Win32_ProcessStartTrace` 进程创建事件（需要管理员权限），但它要经 WMI 服务
中转，本机实测延迟 0.6~1.7 秒，比轮询还慢；因此两者并行、以先到者为准，WMI 只作补充。
界面的「运行状态」会显示当前生效的监听方式。

**`scope`**

- `callsite`：只对 `strategy` 表里记录的该组件调用点生效，其余调用原样放行（生产配置）。
- `global`：对该 API 的所有调用生效（集成自检用，便于在非目标 exe 上验证改写逻辑）。

**`verify_callsite`** 按 `strategy` 的 RVA 读取目标映像，确认那里是一条
`call qword ptr [dll!proc]` 且 IAT 槽已解析到同一函数。exe 版本变化导致 RVA 失配时
直接回退为不改写，不会写坏进程；内存读取以 SEH 保护。

**热重载**：外部编辑并保存配置文件后，程序在下次检查（约 1 s）时自动重载。
若界面里有未保存修改，则忽略外部版本，避免覆盖你正在编辑的内容。

## 扩展一个 hook 组件

1. 在 `src/hook/components/` 新建 `.cpp`，定义 detour 与 `configure` / `verify`；
2. 定义静态 `waf::HkComponent` 实例，`defaultConfig` 写该组件被新加入规则时的默认参数；
3. 用 `HK_REGISTER_COMPONENT` 注册；
4. 在 `CMakeLists.txt` 的 `injector` 目标里加上该源文件。

宿主遍历注册表读配置，不需要改动。界面里的「新增组件」列表来自 `injector.dll` 导出的组件目录
（`HkComponentCount` / `HkComponentName` / `HkComponentDefaultConfig`），因此界面不硬编码任何
组件知识 —— 新组件写好 `defaultConfig` 后会自动出现在列表里。

UI 里「添加规则」按下时使用 `waf::NewDefaultRule()` 的模板（当前含两个组件的默认参数）。

## 自检

```powershell
ctest --preset release                       # YAML/配置的解析与序列化往返
pwsh -File tests\run_integration.ps1         # --inject 单次注入 probe_host，断言改写生效
pwsh -File tests\run_watch_test.ps1          # 常驻宿主轮询 -> 延迟 -> 自动注入（会短暂弹窗）
```

`run_integration.ps1` 通过 `--inject` 让主程序对 `probe_host.exe` 做一次真实注入，断言：
注入后 `rcMonitor` 等于注入前的 `rcWork`，且 `GetDeviceCaps(HORZRES/VERTRES)` 等于工作区宽高。

`run_watch_test.ps1` 走的是后台那条路径：启动常驻宿主，再启动 `probe_host.exe`，
断言宿主在扫描间隔内发现目标、等待 `inject_delay_ms` 后完成注入并装好全部组件。

## 打包

```powershell
cmake --preset release; cmake --build --preset release
ISCC.exe installer\WorkAreaFullscreen.iss
```

产物为 `dist\WorkAreaFullscreen-Setup-<版本>.exe`。安装包内含主程序、`injector.dll` 与
示例配置，可选开机自启与桌面快捷方式；用户配置存放在 `%APPDATA%`，卸载时保留。
