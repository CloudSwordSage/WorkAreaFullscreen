# hookkit

组件式的系统 API 返回值改写框架：`hookkit.dll` 按 YAML 配置 hook 指定 API，
`injector.exe` 按同一份配置启动目标进程并注入。

当前用途：把《Stellaris》无边框全屏窗口的几何来源从显示器整块区域（`rcMonitor`）
改为工作区（`rcWork`），使窗口避开任务栏。定位过程与依据见
`stellaris_borderless_fullscreen_trace.md`。

```
窗口尺寸 <- GDI32!GetDeviceCaps(hdc, HORZRES/VERTRES)  -> SDL desktop_mode
窗口位置 <- USER32!GetMonitorInfoW(rcMonitor)           -> SDL_WINDOWPOS_CENTERED 居中
```

两处同时改为 `work_area` 后，居中结果正好等于 `rcWork`，窗口铺满工作区。

## 目录布局

```
CMakeLists.txt              构建脚本（MSVC x64）
CMakePresets.json           configure/build/test 预设
config/hookkit.yaml         生产配置
include/hookkit/            公共头（组件接口、运行环境、YAML 解析）
  component.h               HkComponent 接口 + 静态注册宏
  runtime.h                 日志、策略调用点表、调用点判定、几何查询
  yamlmini.h                最小 YAML 子集解析器
src/
  hookkit.cpp               宿主：读配置 -> 匹配进程 -> 校验并安装 hook
  runtime.cpp               上述运行环境的实现
  yamlmini.cpp              YAML 解析器实现
  injector.cpp              启动目标并注入
  components/               一个组件一个文件
    get_device_caps.cpp
    get_monitor_info.cpp
tests/
  yamlmini_test.cpp         解析器自检（ctest）
  probe_host.cpp            集成自检的目标进程
  probe.yaml                集成自检配置
  run_integration.ps1       注入并断言改写生效
```

## 构建

需要 MSVC x64 与 MinHook 源码。MinHook 查找顺序：

1. `-DHOOKKIT_MINHOOK_DIR=<dir>`
2. `third_party/minhook`
3. `_mh/minhook-1.3.3`

在 VS Developer 环境（本仓库用 `VSDevShell { ... }`）中：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

产物在 `build/release/`：`hookkit.dll`、`injector.exe`。

## 运行

```powershell
# 工作目录下调用；配置里的 executable 相对当前目录解析
.\build\release\injector.exe config\hookkit.yaml
```

`injector.exe <config.yaml>` 的开关：

| 开关 | 说明 |
| --- | --- |
| `--exe <path>` | 覆盖配置里的 `executable` |
| `--dll <path>` | 覆盖 `hookkit.dll` 路径（默认取 injector 同目录） |
| `--delay <ms>` | 启动到注入的延迟（默认 400；该游戏不支持挂起注入） |
| `--run <ms>` | 注入后等待时长，`0` 表示等到目标退出（默认 0） |

DLL 侧按 `HOOKKIT_CONFIG` -> DLL 同目录 `hookkit.yaml` -> `%TEMP%\hookkit.yaml`
的顺序取配置，日志写入 `%TEMP%\hookkit.log`。

## 配置

```yaml
enabled: true                      # 全局开关

targets:
  - executable: stellaris.exe      # 与当前进程名比对（不区分大小写）
    enabled: true

    strategy: sdl2-gdi             # 提供与该目标版本绑定的已知调用点表（RVA）
    geometry: work_area            # monitor = rcMonitor（等价不改）；work_area = rcWork

    hooks:                         # 键名 = 组件注册名；未列出的组件不安装
      GetDeviceCaps:
        enabled: true
        indices:                   # 仅这些索引被改写
          - HORZRES
          - VERTRES
        scope: callsite

      GetMonitorInfoW:
        enabled: true
        fields:                    # dest: src，把 dest 字段复制为 src 字段
          rcMonitor: rcWork
        scope: callsite

    safety:
      fallback: passthrough        # 任何条件不满足时的行为
      verify_callsite: true        # 安装前校验调用点字节与 IAT 槽
```

**`scope`**

- `callsite`：只对 `strategy` 表里记录的该组件调用点生效，其余调用原样放行。生产配置用这个。
- `global`：对该 API 的所有调用生效（集成自检用，便于在非目标 exe 上验证改写逻辑）。

**`verify_callsite`** 按 `strategy` 的 RVA 读取目标映像，确认那里是一条
`call qword ptr [dll!proc]` 且 IAT 槽已解析到同一函数。exe 版本变化导致 RVA 失配时
直接回退 passthrough，不会写坏进程。内存读取以 SEH 保护。

> 解析器只支持 YAML 的一个子集：块状映射与序列、行内注释、引号标量；
> 不支持流式写法（`[]` / `{}`）、锚点与多行标量。

## 扩展一个 hook 组件

1. 在 `src/components/` 新建 `.cpp`，定义 detour 与 `configure` / `verify`；
2. 定义静态 `HkComponent` 实例；
3. 用 `HK_REGISTER_COMPONENT` 注册；
4. 在 `CMakeLists.txt` 的 `hookkit` 目标里加上该源文件。

宿主遍历注册表读配置，不需要改动。若目标版本的调用点是新发现的，把 RVA 加进
`src/runtime.cpp` 的策略表（`Strategies()`）即可，`verify_callsite` 会自动复核。

参考 `src/components/get_monitor_info.cpp`：约 100 行，含全部接线。

## 自检

```powershell
ctest --preset release                       # 1) YAML 解析器自检
pwsh -File tests\run_integration.ps1         # 2) 注入 probe_host，断言改写生效
```

`run_integration.ps1` 断言：注入后 `rcMonitor` 等于注入前的 `rcWork`，
且 `GetDeviceCaps(HORZRES/VERTRES)` 等于工作区宽高。
