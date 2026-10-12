// dllmain.cpp - 被注入目标的 hook 宿主：读配置 -> 匹配当前进程 -> 安装组件
// hook。
//
// 所有行为由配置决定；组件在 src/hook/components/ 下，一个文件一个 hook。
#include <windows.h>

#include <stdio.h>
#include <string>

#include "MinHook.h"
#include "waf/component.h"
#include "waf/config.h"
#include "waf/config_io.h"
#include "waf/runtime.h"

namespace waf {
    namespace {

        std::wstring CurrentExeName() {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            const wchar_t * sep = wcsrchr(path, L'\\');
            return sep ? std::wstring(sep + 1) : std::wstring(path);
        }

        // 配置的定位与读取统一由 waf/config_io 提供（启动代理也用同一份实现）。

        std::string NarrowA(const std::wstring & w) {
            if (w.empty())
                return std::string();
            const int n = WideCharToMultiByte(
                CP_ACP,
                0,
                w.c_str(),
                (int)w.size(),
                nullptr,
                0,
                nullptr,
                nullptr
            );
            std::string s(static_cast<size_t>(n), '\0');
            if (n > 0)
                WideCharToMultiByte(
                    CP_ACP,
                    0,
                    w.c_str(),
                    (int)w.size(),
                    &s[0],
                    n,
                    nullptr,
                    nullptr
                );
            return s;
        }

        /** 目标进程已运行的时间（毫秒）。注入时机是否够早，看这一个数。 */
        ULONGLONG TargetUptimeMs() {
            FILETIME create{}, exitTime{}, kernel{}, user{};
            if (!GetProcessTimes(
                    GetCurrentProcess(),
                    &create,
                    &exitTime,
                    &kernel,
                    &user
                ))
                return 0;

            FILETIME now{};
            GetSystemTimeAsFileTime(&now);

            ULARGE_INTEGER c, n;
            c.LowPart = create.dwLowDateTime;
            c.HighPart = create.dwHighDateTime;
            n.LowPart = now.dwLowDateTime;
            n.HighPart = now.dwHighDateTime;
            return (n.QuadPart > c.QuadPart)
                       ? (n.QuadPart - c.QuadPart) / 10000ull
                       : 0;
        }

        /** 让 SDL 重建显示器缓存。
         *
         * SDL 只在视频初始化（WIN_InitModes）或收到 WM_DISPLAYCHANGE
         * 时才枚举显示器，并把 GetDeviceCaps(HORZRES/VERTRES) 的结果缓存进
         * display.desktop_mode。注入通常晚于视频初始化，
         * 缓存里已经是整屏尺寸；这里主动触发一次重新枚举，让缓存按（被 hook
         * 改写的）工作区尺寸重建。
         *
         * 必须等目标出现 SDL 窗口再发：刷新依赖窗口过程 WIN_WindowProc
         * 处理该消息。
         */
        DWORD WINAPI RefreshDisplaysThread(LPVOID) {
            for (int i = 0; i < 2400; ++i) { // 最多等 120 秒（50ms x 2400）
                HWND sdl = nullptr;
                EnumWindows(
                    [](HWND hwnd, LPARAM lp) -> BOOL {
                        wchar_t cls[64] = {0};
                        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) &&
                            wcscmp(cls, L"SDL_app") == 0) {
                            *reinterpret_cast<HWND *>(lp) = hwnd;
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&sdl)
                );

                if (sdl) {
                    DWORD_PTR unused = 0;
                    // ABORTIFHUNG：目标消息泵若卡住，不要把这个线程也一起拖住
                    if (SendMessageTimeoutW(
                            sdl,
                            WM_DISPLAYCHANGE,
                            0,
                            0,
                            SMTO_ABORTIFHUNG,
                            5000,
                            &unused
                        )) {
                        HkLog(
                            "已向 SDL 窗口 %p 发送 "
                            "WM_DISPLAYCHANGE，触发显示器重新枚举",
                            sdl
                        );
                    } else {
                        HkLog(
                            "WM_DISPLAYCHANGE 发送失败（err=%lu）",
                            GetLastError()
                        );
                    }
                    return 0;
                }
                Sleep(50);
            }
            HkLog("等待超时：未发现 SDL 窗口，未触发显示器重新枚举");
            return 0;
        }

        DWORD WINAPI InitThread(LPVOID param) {
            HkLogOpen();

            const std::wstring exeName = CurrentExeName();

            // DLL 自身所在目录，用于便携部署时定位配置
            wchar_t self[MAX_PATH]{};
            GetModuleFileNameW(static_cast<HMODULE>(param), self, MAX_PATH);
            std::wstring selfDir(self);
            {
                const size_t sep = selfDir.find_last_of(L"\\/");
                selfDir = (sep == std::wstring::npos) ? std::wstring()
                                                      : selfDir.substr(0, sep);
            }

            const std::wstring cfgPath = waf::FindConfigPath(selfDir);
            HkLog(
                "target process: %s（已运行 %llu ms）",
                NarrowA(exeName).c_str(),
                TargetUptimeMs()
            );
            HkLog("config: %s", NarrowA(cfgPath).c_str());

            std::string text;
            if (!waf::ReadAllUtf8(cfgPath, text) || text.empty()) {
                HkLog("config missing or unreadable -> passthrough");
                return 0;
            }

            waf::AppConfig cfg;
            std::string err;
            if (!waf::AppConfig::Parse(text, cfg, err)) {
                HkLog("config parse error: %s -> passthrough", err.c_str());
                return 0;
            }
            if (!cfg.enabled) {
                HkLog("config disabled globally -> passthrough");
                return 0;
            }

            const waf::TargetRule * rule = cfg.FindRule(NarrowA(exeName));
            if (!rule) {
                HkLog("no enabled rule matches this process -> passthrough");
                return 0;
            }

            HkEnv env;
            env.targetMod = GetModuleHandleW(nullptr);
            env.strategy = rule->strategy;
            env.geometry = rule->geometry;
            HkLog(
                "rule matched: strategy=%s geometry=%s",
                env.strategy.c_str(),
                env.geometry.c_str()
            );

            const MH_STATUS ms = MH_Initialize();
            HkLog("MH_Initialize -> %d", (int)ms);
            if (ms != MH_OK)
                return 0;

            int installed = 0;
            for (const waf::HkComponent * c : waf::HkRegistry()) {
                const waf::HookSetting * hs = nullptr;
                for (const auto & h : rule->hooks)
                    if (h.name == c->name) {
                        hs = &h;
                        break;
                    }

                if (!hs) {
                    HkLog("%s: not configured -> skip", c->name);
                    continue;
                }
                if (!hs->enabled) {
                    HkLog("%s: disabled", c->name);
                    continue;
                }
                if (!c->configure(*hs, env)) {
                    HkLog("%s: configure rejected -> passthrough", c->name);
                    continue;
                }
                if (rule->verifyCallsite && !c->verify(env)) {
                    HkLog("%s: callsite verify FAILED -> passthrough", c->name);
                    continue;
                }

                const MH_STATUS s =
                    MH_CreateHookApi(c->dll, c->proc, c->detour, c->trampoline);
                if (s != MH_OK) {
                    HkLog(
                        "%s: MH_CreateHookApi -> %d -> passthrough",
                        c->name,
                        (int)s
                    );
                    continue;
                }
                HkLog("%s: hook created", c->name);
                ++installed;
            }

            // 配置里写了但未注册的组件名：记录，便于扩展时发现拼写错误
            for (const auto & h : rule->hooks) {
                bool known = false;
                for (const waf::HkComponent * c : waf::HkRegistry())
                    if (h.name == c->name) {
                        known = true;
                        break;
                    }
                if (!known)
                    HkLog(
                        "warning: unknown hook component '%s' (ignored)",
                        h.name.c_str()
                    );
            }

            if (installed > 0)
                HkLog("MH_EnableHook -> %d", (int)MH_EnableHook(MH_ALL_HOOKS));
            HkLog("=== %d component(s) active ===", installed);

            // 装了 hook 才值得刷新缓存：刷新时 GetDeviceCaps 必须已经被接住。
            if (installed > 0 && env.strategy == "sdl2-gdi") {
                HANDLE t = CreateThread(
                    nullptr,
                    0,
                    RefreshDisplaysThread,
                    nullptr,
                    0,
                    nullptr
                );
                if (t)
                    CloseHandle(t);
            }
            return 0;
        }

    } // namespace
} // namespace waf

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // 宿主进程也会加载本 DLL，但只是为了读组件目录（见下面的 HkComponent*
        // 导出）。 那种加载绝不能在这里装 hook。
        wchar_t flag[8] = {0};
        if (GetEnvironmentVariableW(L"HOOKKIT_QUERY", flag, ARRAYSIZE(flag)) >
            0)
            return TRUE;

        // 注入发生在目标运行中，初始化放到独立线程，避免在 DllMain
        // 内触发加载器锁问题。
        HANDLE t =
            CreateThread(nullptr, 0, waf::InitThread, hModule, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}

// ---- 组件目录导出 ----
// 宿主界面据此列出“可添加的组件”及其默认参数，避免界面硬编码组件知识；
// 新增组件时只要写好 defaultConfig，界面自动就能列出。

extern "C" __declspec(dllexport) int HkComponentCount(void) {
    return static_cast<int>(waf::HkRegistry().size());
}

extern "C" __declspec(dllexport) const char * HkComponentName(int index) {
    const auto & reg = waf::HkRegistry();
    if (index < 0 || index >= static_cast<int>(reg.size()))
        return nullptr;
    return reg[static_cast<size_t>(index)]->name;
}

extern "C" __declspec(dllexport) const char * HkComponentDefaultConfig(
    int index
) {
    const auto & reg = waf::HkRegistry();
    if (index < 0 || index >= static_cast<int>(reg.size()))
        return nullptr;
    const char * cfg = reg[static_cast<size_t>(index)]->defaultConfig;
    return cfg ? cfg : "";
}
