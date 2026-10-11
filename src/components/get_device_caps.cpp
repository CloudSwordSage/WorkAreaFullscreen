// get_device_caps.cpp - 组件：按几何语义改写 GetDeviceCaps 的尺寸类索引。
//
// 用途：SDL 的 WIN_UpdateDisplayMode 以 GetDeviceCaps(hdc, HORZRES/VERTRES) 作为
// display.desktop_mode，引擎再取该尺寸作为窗口宽高。改写为工作区宽高后，配合
// rcMonitor->rcWork 的居中结果，窗口恰好铺满工作区。
// 参见 stellaris_borderless_fullscreen_trace.md 第 9.4 节。
#include <intrin.h>

#include <string>
#include <vector>

#include "hookkit/component.h"

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

int WINAPI Detour_GetDeviceCaps(HDC hdc, int idx)
{
    const int v = o_GetDeviceCaps(hdc, idx);

    if (idx != HORZRES && idx != VERTRES) return v;
    if ((idx == HORZRES && !g_horz) || (idx == VERTRES && !g_vert)) return v;

    if (g_callsiteScope &&
        !HkFromKnownCallsite(g_env, "GetDeviceCaps", _ReturnAddress())) return v;

    // 无法定位 hdc 所属监视器时放行原值（fallback: passthrough）。
    RECT r;
    if (!HkQueryGeometryRect(g_env, hdc, &r)) return v;

    return (idx == HORZRES) ? (r.right - r.left) : (r.bottom - r.top);
}

bool Configure(const yamlmini::Node& cfg, const HkEnv& env)
{
    g_env = env;

    g_callsiteScope = true;
    if (const yamlmini::Node* scope = cfg.find("scope"))
        g_callsiteScope = (scope->str("callsite") == "callsite");

    g_horz = g_vert = false;
    if (const yamlmini::Node* indices = cfg.find("indices")) {
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
    Configure,
    Verify,
};

} // namespace

HK_REGISTER_COMPONENT(g_component);
