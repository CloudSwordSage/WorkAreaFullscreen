// enum_display_settings.cpp - 组件：把“当前显示模式”的像素尺寸改成工作区尺寸。
//
// 为什么需要它：SDL 的 display_modes 列表由 WIN_GetDisplayModes 逐项调用
// EnumDisplaySettingsW(deviceName, i, ...) 填充，那一路径不经过 GetDeviceCaps
// （后者只在 index == ENUM_CURRENT_SETTINGS 时被 WIN_UpdateDisplayMode 使用）。
// 引擎若取列表首项（Windows 把当前模式排在 index 0）作为窗口宽高，只改
// GetDeviceCaps 是够不到的，必须同时改这里。
#include <intrin.h>

#include <string>
#include <vector>

#include "waf/component.h"

namespace waf {
    namespace {

        typedef BOOL(WINAPI * PFN_EnumDisplaySettingsW)(
            LPCWSTR,
            DWORD,
            DEVMODEW *
        );

        PFN_EnumDisplaySettingsW o_EnumDisplaySettingsW = nullptr;

        HkEnv g_env;
        bool g_callsiteScope = true;

        volatile LONG g_hitLeft = 20;
        volatile LONG g_skipLeft = 8;

        BOOL WINAPI
        Detour_EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW * dm) {
            const BOOL r = o_EnumDisplaySettingsW(device, mode, dm);
            if (!r || !dm || !device)
                return r;

            if (g_callsiteScope && !HkFromKnownCallsite(
                                       g_env,
                                       "EnumDisplaySettingsW",
                                       _ReturnAddress()
                                   ))
                return r;

            // 只动“当前模式”：ENUM_CURRENT_SETTINGS，以及枚举列表的第一项
            // （EnumDisplaySettingsW 把当前模式排在 index 0）。
            if (mode != ENUM_CURRENT_SETTINGS && mode != 0)
                return r;

            const HMONITOR mon = HkMonitorOfDevice(device);
            RECT target;
            if (!mon || !HkQueryGeometryRect(g_env, mon, &target)) {
                if (InterlockedDecrement(&g_skipLeft) >= 0)
                    HkLog(
                        "EnumDisplaySettingsW(%ls mode=%lu) "
                        "无法定位显示器，未改写",
                        device,
                        mode
                    );
                return r;
            }

            const DWORD w = static_cast<DWORD>(target.right - target.left);
            const DWORD h = static_cast<DWORD>(target.bottom - target.top);
            if (InterlockedDecrement(&g_hitLeft) >= 0)
                HkLog(
                    "EnumDisplaySettingsW(%ls mode=%lu) %lux%lu -> "
                    "%lux%lu（工作区 %ld,%ld-%ld,%ld）",
                    device,
                    mode,
                    dm->dmPelsWidth,
                    dm->dmPelsHeight,
                    w,
                    h,
                    target.left,
                    target.top,
                    target.right,
                    target.bottom
                );

            dm->dmPelsWidth = w;
            dm->dmPelsHeight = h;
            dm->dmFields |= (DM_PELSWIDTH | DM_PELSHEIGHT);
            return r;
        }

        bool Configure(const HookSetting & s, const HkEnv & env) {
            g_env = env;
            g_callsiteScope = (s.scope != "global");
            HkLog(
                "EnumDisplaySettingsW: scope=%s",
                g_callsiteScope ? "callsite" : "global"
            );
            return true;
        }

        bool Verify(const HkEnv & env) {
            const std::vector<ULONG_PTR> & cs =
                HkStrategyCallsites(env.strategy, "EnumDisplaySettingsW");
            if (cs.empty()) {
                HkLog(
                    "EnumDisplaySettingsW: 策略表无该组件调用点，跳过调用点校验"
                );
                return true;
            }
            for (ULONG_PTR rva : cs) {
                if (!HkVerifyCallsite(
                        env,
                        rva,
                        L"user32.dll",
                        "EnumDisplaySettingsW"
                    )) {
                    HkLog(
                        "EnumDisplaySettingsW: callsite 0x%llX mismatch",
                        (unsigned long long)rva
                    );
                    return false;
                }
            }
            return true;
        }

        HkComponent g_component = {
            "EnumDisplaySettingsW",
            L"user32.dll",
            "EnumDisplaySettingsW",
            reinterpret_cast<void *>(Detour_EnumDisplaySettingsW),
            reinterpret_cast<void **>(&o_EnumDisplaySettingsW),
            "scope: global\n", // 通用默认：新规则不绑定策略表；群星默认见 DefaultConfig
            Configure,
            Verify,
        };

    } // namespace

    HK_REGISTER_COMPONENT(g_component);

} // namespace waf
