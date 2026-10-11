// runtime.cpp - 日志、策略调用点表、调用点判定与几何查询。
#include "waf/runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace waf {
namespace {

// ---------------- 日志 ----------------
HANDLE           g_log     = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_cs;
bool             g_csReady = false;
DWORD            g_t0      = 0;

// ---------------- 策略表 ----------------
struct CallsiteEntry {
    const char*               component;
    std::vector<ULONG_PTR>    rvas;
};
struct StrategyEntry {
    const char*               name;
    std::vector<CallsiteEntry> entries;
};

/**
 * 内置策略表。
 *
 * RVA 为调用指令地址，取自 stellaris_borderless_fullscreen_trace.md 第 6 与 9.4 节，
 * 绑定 stellaris.exe + 静态链接的 SDL 2.26.5 特定版本。安装前的 verify_callsite 会用
 * 指令字节与 IAT 槽复核这些值，不一致即回退，所以版本变化不会造成误改。
 */
const std::vector<StrategyEntry>& Strategies()
{
    static const std::vector<StrategyEntry> s = {
        { "sdl2-gdi", {
            { "GetMonitorInfoW", { 0x1DFAAF5, 0x1DFABA1, 0x1DFB491, 0x1DFE184 } },
            { "GetDeviceCaps",   { 0x1DFBB35, 0x1DFBB45 } },
        } },
    };
    return s;
}

bool SafeReadPtr(void* const* slot, void** out)
{
    __try {
        *out = *slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HMONITOR MonitorOfDC(HDC hdc)
{
    POINT pt = { 0, 0 };
    // 显示器 DC 的最终原点即其在虚拟桌面中的位置；取不到时退回主显示器。
    if (hdc && GetDCOrgEx(hdc, &pt)) {
        HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
        if (m) return m;
    }
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

bool RectOfMonitor(HMONITOR mon, const std::string& geometry, RECT* out)
{
    if (!mon || !out) return false;

    // 用 ANSI 变体读取：GetMonitorInfoW 自身可能已被组件 hook，走 W 会在 scope=global
    // 时把内部查询也改写掉（自伤）。A/W 是两个独立实现，未被 hook。
    // 注意 MONITORINFO 没有 A/W 之分（结构内无字符字段），GetMonitorInfoA 也用该类型。
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoA(mon, &mi)) return false;

    if (geometry == "work_area") { *out = mi.rcWork;    return true; }
    if (geometry == "monitor")   { *out = mi.rcMonitor; return true; }
    return false;
}

} // namespace

void HkLogOpen()
{
    char  path[MAX_PATH] = { 0 };
    DWORD n              = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 12) lstrcpyA(path, "C:\\Windows\\Temp\\");
    lstrcatA(path, "WorkAreaFullscreen-hook.log");

    g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    InitializeCriticalSection(&g_cs);
    g_csReady = true;
    g_t0      = GetTickCount();
    HkLog("=== hookkit log opened: %s ===", path);
}

void HkLog(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;

    char    body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    char line[1200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%6lu ms] %-5lu %s\r\n",
                GetTickCount() - g_t0, GetCurrentThreadId(), body);

    DWORD w = 0;
    if (g_csReady) EnterCriticalSection(&g_cs);
    WriteFile(g_log, line, static_cast<DWORD>(strlen(line)), &w, NULL);
    if (g_csReady) LeaveCriticalSection(&g_cs);
}

const std::vector<ULONG_PTR>& HkStrategyCallsites(const std::string& strategy,
                                                  const std::string& component)
{
    static const std::vector<ULONG_PTR> kEmpty;
    for (const auto& st : Strategies()) {
        if (strategy != st.name) continue;
        for (const auto& e : st.entries)
            if (component == e.component) return e.rvas;
    }
    return kEmpty;
}

bool HkFromKnownCallsite(const HkEnv& env, const std::string& component, void* returnAddress)
{
    if (!env.targetMod || !returnAddress) return false;

    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(env.targetMod);
    const ULONG_PTR ra   = reinterpret_cast<ULONG_PTR>(returnAddress);
    if (ra < base) return false;

    // 表内记录调用指令地址；`call qword ptr [rip+disp32]` 长 6 字节，故返回地址 = 地址 + 6。
    const ULONG_PTR rva = ra - base;
    for (ULONG_PTR cs : HkStrategyCallsites(env.strategy, component))
        if (rva == cs + 6) return true;
    return false;
}

bool HkVerifyCallsite(const HkEnv& env, ULONG_PTR rva, const wchar_t* dll, const char* proc)
{
    if (!env.targetMod) return false;

    HMODULE mod = GetModuleHandleW(dll);
    if (!mod) return false;
    FARPROC real = GetProcAddress(mod, proc);
    if (!real) return false;

    const BYTE* p = reinterpret_cast<const BYTE*>(env.targetMod) + rva;

    __try {
        if (p[0] != 0xFF || p[1] != 0x15) return false;   // call qword ptr [rip+disp32]

        INT32 disp = 0;
        memcpy(&disp, p + 2, sizeof(disp));

        void* target = nullptr;
        if (!SafeReadPtr(reinterpret_cast<void* const*>(p + 6 + disp), &target)) return false;
        return target == reinterpret_cast<void*>(real);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HkQueryGeometryRect(const HkEnv& env, HMONITOR mon, RECT* out)
{
    return RectOfMonitor(mon, env.geometry, out);
}

bool HkQueryGeometryRect(const HkEnv& env, HDC hdc, RECT* out)
{
    return RectOfMonitor(MonitorOfDC(hdc), env.geometry, out);
}

HMONITOR HkMonitorOfDevice(const std::wstring& deviceName)
{
    char narrow[64] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, deviceName.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);

    struct Ctx {
        const char* name;
        HMONITOR    found;
    };
    Ctx ctx{ narrow, nullptr };

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        // 用 A 变体：GetMonitorInfoW 可能已被本框架改写（szDevice 不受影响，但保持读取路径干净）
        MONITORINFOEXA mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(mon, reinterpret_cast<LPMONITORINFO>(&mi)) &&
            _stricmp(mi.szDevice, c->name) == 0) {
            c->found = mon;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.found;
}

} // namespace waf
