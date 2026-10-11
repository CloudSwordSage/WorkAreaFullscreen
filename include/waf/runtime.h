// runtime.h - 宿主提供给 hook 组件的运行环境、策略表与几何查询工具。
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace waf {

/** 打开 %TEMP%\WorkAreaFullscreen-hook.log（覆盖写）；DLL 初始化早期调用一次。 */
void HkLogOpen();

/** 追加一条带时间戳与线程号的日志；未打开日志时为空操作。 */
void HkLog(const char* fmt, ...);

/** 组件运行环境：安装期由宿主填充，运行期只读。 */
struct HkEnv {
    HMODULE     targetMod = nullptr;  ///< 目标进程主模块；callsite RVA 的基准
    std::string strategy;             ///< 策略名，如 "sdl2-gdi"
    std::string geometry;             ///< 几何语义："monitor" | "work_area"
};

/**
 * 查询策略内置的调用点 RVA 表。
 *
 * @param strategy  策略名。
 * @param component 组件名（与 HkComponent::name 一致）。
 * @return 调用指令 RVA 列表；未知策略或组件时返回空表。
 */
const std::vector<ULONG_PTR>& HkStrategyCallsites(const std::string& strategy,
                                                  const std::string& component);

/**
 * 判断某次 API 调用是否来自策略已知的调用点。
 *
 * @param env           运行环境。
 * @param component     组件名。
 * @param returnAddress 被 hook API 调用方的返回地址；须在 detour 内直接取 _ReturnAddress()，
 *                      否则取到的是 helper 自身的返回地址。
 * @return 命中已知调用点返回 true。
 */
bool HkFromKnownCallsite(const HkEnv& env, const std::string& component, void* returnAddress);

/**
 * 硬校验目标映像中某 RVA 处确实是一条 `call [dll!proc]`。
 *
 * 用于在安装前确认 exe 版本与策略表一致；任一环节不符返回 false，调用方应走 fallback。
 * 内部以 SEH 保护内存读取，地址失效不会导致进程崩溃。
 *
 * @param env  运行环境。
 * @param rva  相对目标模块基址的调用指令地址。
 * @param dll  目标 DLL（如 L"user32.dll"）。
 * @param proc 目标导出名。
 * @return 指令形态匹配且 IAT 槽已解析到同一函数时返回 true。
 */
bool HkVerifyCallsite(const HkEnv& env, ULONG_PTR rva, const wchar_t* dll, const char* proc);

/** 按几何语义求指定监视器的目标矩形。 @return 无法确定时返回 false。 */
bool HkQueryGeometryRect(const HkEnv& env, HMONITOR mon, RECT* out);

/**
 * 按几何语义求指定显示器 DC 所属监视器的目标矩形。
 *
 * 经 GetDCOrgEx + MonitorFromPoint 定位监视器，失败时退回主显示器。
 * 内部会调用 GetMonitorInfoW，因此被 hook 时经由 detour 走 passthrough 分支，结果不变。
 */
bool HkQueryGeometryRect(const HkEnv& env, HDC hdc, RECT* out);

} // namespace waf
