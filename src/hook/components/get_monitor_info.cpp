// get_monitor_info.cpp - 组件：按配置映射 MONITORINFO 的矩形字段。
//
// 用途：SDL 的 WIN_GetDisplayBounds 以 rcMonitor 产出显示器边界，SDL_CreateWindow
// 再用它解析 SDL_WINDOWPOS_CENTERED。把 rcMonitor 改写为 rcWork 即可让居中结果落在工作区。
// 参见 stellaris_borderless_fullscreen_trace.md 第 6 与 9.4 节。
#include <intrin.h>

#include <string>
#include <utility>
#include <vector>

#include "waf/component.h"

namespace waf {
namespace {

typedef BOOL(WINAPI* PFN_GetMonitorInfoW)(HMONITOR, LPMONITORINFO);

PFN_GetMonitorInfoW o_GetMonitorInfoW = nullptr;

enum MonitorField { MF_MONITOR, MF_WORK };

HkEnv                       g_env;
bool                        g_callsiteScope = true;
std::vector<std::pair<int, int>> g_fields;   // (dest, src)

bool FieldId(const std::string& s, int* out)
{
    if (s == "rcMonitor") { *out = MF_MONITOR; return true; }
    if (s == "rcWork")    { *out = MF_WORK;    return true; }
    return false;
}

RECT* FieldOf(LPMONITORINFO mi, int id)
{
    return (id == MF_MONITOR) ? &mi->rcMonitor : &mi->rcWork;
}

const char* FieldName(int id) { return (id == MF_MONITOR) ? "rcMonitor" : "rcWork"; }

volatile LONG g_hitLeft  = 20;
volatile LONG g_missLeft = 8;

BOOL WINAPI Detour_GetMonitorInfoW(HMONITOR mon, LPMONITORINFO mi)
{
    const BOOL r = o_GetMonitorInfoW(mon, mi);
    if (!r || !mi || mi->cbSize < sizeof(MONITORINFO)) return r;

    // 只在策略已知的调用点改写，其余（含本进程内部查询）原样放行。
    void* ra = _ReturnAddress();
    if (g_callsiteScope && !HkFromKnownCallsite(g_env, "GetMonitorInfoW", ra)) {
        if (InterlockedDecrement(&g_missLeft) >= 0)
            HkLog("GetMonitorInfoW(mon=%p) 调用点不在策略表中（rva=0x%llX），未改写",
                  mon, (unsigned long long)((ULONG_PTR)ra - (ULONG_PTR)g_env.targetMod));
        return r;
    }

    for (const auto& f : g_fields) {
        RECT* dst = FieldOf(mi, f.first);
        RECT* src = FieldOf(mi, f.second);
        if (dst == src) continue;
        if (InterlockedDecrement(&g_hitLeft) >= 0)
            HkLog("GetMonitorInfoW(mon=%p) %s=(%ld,%ld)-(%ld,%ld) -> %s=(%ld,%ld)-(%ld,%ld)",
                  mon, FieldName(f.first), dst->left, dst->top, dst->right, dst->bottom,
                  FieldName(f.second), src->left, src->top, src->right, src->bottom);
        *dst = *src;
    }
    return r;
}

bool Configure(const HookSetting& s, const HkEnv& env)
{
    g_env = env;

    g_callsiteScope = (s.scope != "global");

    g_fields.clear();
    if (const yamlmini::Node* fields = s.params.find("fields")) {
        if (fields->kind == yamlmini::Node::Map) {
            for (const auto& kv : fields->map) {
                int dst = 0, src = 0;
                if (FieldId(kv.first, &dst) && FieldId(kv.second.str(), &src))
                    g_fields.emplace_back(dst, src);
                else
                    HkLog("GetMonitorInfoW: unknown field map '%s: %s'",
                          kv.first.c_str(), kv.second.str().c_str());
            }
        } else {
            HkLog("GetMonitorInfoW: 'fields' must be a map");
        }
    }
    // 未显式配置字段映射时，用几何语义推导。
    if (g_fields.empty() && env.geometry == "work_area")
        g_fields.emplace_back(MF_MONITOR, MF_WORK);

    if (g_fields.empty()) {
        HkLog("GetMonitorInfoW: no field mapping configured");
        return false;
    }
    HkLog("GetMonitorInfoW: %zu field map(s), scope=%s",
          g_fields.size(), g_callsiteScope ? "callsite" : "global");
    return true;
}

bool Verify(const HkEnv& env)
{
    const std::vector<ULONG_PTR>& cs = HkStrategyCallsites(env.strategy, "GetMonitorInfoW");
    if (cs.empty()) { HkLog("GetMonitorInfoW: strategy has no callsite table"); return false; }

    for (ULONG_PTR rva : cs) {
        if (!HkVerifyCallsite(env, rva, L"user32.dll", "GetMonitorInfoW")) {
            HkLog("GetMonitorInfoW: callsite 0x%llX mismatch", (unsigned long long)rva);
            return false;
        }
    }
    return true;
}

HkComponent g_component = {
    "GetMonitorInfoW",
    L"user32.dll",
    "GetMonitorInfoW",
    reinterpret_cast<void*>(Detour_GetMonitorInfoW),
    reinterpret_cast<void**>(&o_GetMonitorInfoW),
    Configure,
    Verify,
};

} // namespace

HK_REGISTER_COMPONENT(g_component);

} // namespace waf
