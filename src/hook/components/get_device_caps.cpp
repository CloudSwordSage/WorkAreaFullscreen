// get_device_caps.cpp - 组件：按几何语义改写 GetDeviceCaps 的尺寸类索引。
//
// 用途：SDL 的 WIN_UpdateDisplayMode 以 GetDeviceCaps(hdc, HORZRES/VERTRES) 作为
// display.desktop_mode，引擎再取该尺寸作为窗口宽高。改写为工作区宽高后，配合
// rcMonitor->rcWork 的居中结果，窗口恰好铺满工作区。
// 定位过程见 docs/stellaris.md。
#include <intrin.h>

#include <string>
#include <vector>

#include "waf/component.h"

namespace waf {
namespace {

typedef int(WINAPI* PFN_GetDeviceCaps)(HDC, int);

PFN_GetDeviceCaps o_GetDeviceCaps = nullptr;

HkEnv g_env;
bool  g_callsiteScope = true;
bool  g_horz          = false;   // HORZRES
bool  g_vert          = false;   // VERTRES

bool IndexId(const std::string& s, int* out)
{
    if (s == "HORZRES") { *out = HORZRES; return true; }
    if (s == "VERTRES") { *out = VERTRES; return true; }
    return false;
}

const char* IndexName(int idx) { return (idx == HORZRES) ? "HORZRES" : "VERTRES"; }

// 诊断日志限次，避免每帧刷屏
volatile LONG g_hitLeft  = 20;
volatile LONG g_missLeft = 8;
volatile LONG g_failLeft = 4;

int WINAPI Detour_GetDeviceCaps(HDC hdc, int idx)
{
    const int v = o_GetDeviceCaps(hdc, idx);

    if (idx != HORZRES && idx != VERTRES) return v;
    if ((idx == HORZRES && !g_horz) || (idx == VERTRES && !g_vert)) return v;

    if (g_callsiteScope) {
        void* ra = _ReturnAddress();
        if (!HkFromKnownCallsite(g_env, "GetDeviceCaps", ra)) {
            if (InterlockedDecrement(&g_missLeft) >= 0)
                HkLog("GetDeviceCaps(%s) 调用点不在策略表中（rva=0x%llX），未改写",
                      IndexName(idx),
                      (unsigned long long)((ULONG_PTR)ra - (ULONG_PTR)g_env.targetMod));
            return v;
        }
    }

    // 无法定位 hdc 所属监视器时放行原值（fallback: passthrough）。
    RECT r;
    if (!HkQueryGeometryRect(g_env, hdc, &r)) {
        if (InterlockedDecrement(&g_failLeft) >= 0)
            HkLog("GetDeviceCaps(%s) 无法定位 hdc 所属显示器，未改写", IndexName(idx));
        return v;
    }

    const int nv = (idx == HORZRES) ? (r.right - r.left) : (r.bottom - r.top);
    if (InterlockedDecrement(&g_hitLeft) >= 0)
        HkLog("GetDeviceCaps(%s) hdc=%p %d -> %d（工作区 %ld,%ld-%ld,%ld）",
              IndexName(idx), hdc, v, nv, r.left, r.top, r.right, r.bottom);
    return nv;
}

bool Configure(const HookSetting& s, const HkEnv& env)
{
    g_env = env;

    g_callsiteScope = (s.scope != "global");

    g_horz = g_vert = false;
    if (const yamlmini::Node* indices = s.params.find("indices")) {
        if (indices->kind == yamlmini::Node::Seq) {
            for (const auto& it : indices->seq) {
                int id = 0;
                if (!IndexId(it.str(), &id)) {
                    HkLog("GetDeviceCaps: unknown index '%s'", it.str().c_str());
                    continue;
                }
                if (id == HORZRES) g_horz = true;
                if (id == VERTRES) g_vert = true;
            }
        } else {
            HkLog("GetDeviceCaps: 'indices' must be a sequence");
        }
    }

    if (!g_horz && !g_vert) {
        HkLog("GetDeviceCaps: no index configured");
        return false;
    }
    HkLog("GetDeviceCaps: HORZRES=%d VERTRES=%d scope=%s",
          (int)g_horz, (int)g_vert, g_callsiteScope ? "callsite" : "global");
    return true;
}

bool Verify(const HkEnv& env)
{
    const std::vector<ULONG_PTR>& cs = HkStrategyCallsites(env.strategy, "GetDeviceCaps");
    if (cs.empty()) { HkLog("GetDeviceCaps: strategy has no callsite table"); return false; }

    for (ULONG_PTR rva : cs) {
        if (!HkVerifyCallsite(env, rva, L"gdi32.dll", "GetDeviceCaps")) {
            HkLog("GetDeviceCaps: callsite 0x%llX mismatch", (unsigned long long)rva);
            return false;
        }
    }
    return true;
}

HkComponent g_component = {
    "GetDeviceCaps",
    L"gdi32.dll",
    "GetDeviceCaps",
    reinterpret_cast<void*>(Detour_GetDeviceCaps),
    reinterpret_cast<void**>(&o_GetDeviceCaps),
    "indices:\n"
    "  - HORZRES\n"
    "  - VERTRES\n",
    Configure,
    Verify,
};

} // namespace

HK_REGISTER_COMPONENT(g_component);

} // namespace waf
