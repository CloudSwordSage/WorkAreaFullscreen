// dllmain.cpp - 被注入目标的 hook 宿主：读配置 -> 匹配当前进程 -> 安装组件 hook。
//
// 所有行为由配置决定；组件在 src/hook/components/ 下，一个文件一个 hook。
#include <windows.h>

#include <stdio.h>
#include <string>

#include "MinHook.h"
#include "waf/component.h"
#include "waf/config.h"
#include "waf/runtime.h"

namespace waf {
namespace {

std::wstring CurrentExeName()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* sep = wcsrchr(path, L'\\');
    return sep ? std::wstring(sep + 1) : std::wstring(path);
}

// 配置查找顺序：HOOKKIT_CONFIG > 与 DLL 同目录（便携部署）> %APPDATA%\WorkAreaFullscreen > %TEMP%。
std::wstring FindConfig(HMODULE selfMod)
{
    wchar_t buf[MAX_PATH]{};
    DWORD   n = GetEnvironmentVariableW(L"HOOKKIT_CONFIG", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;

    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(selfMod, self, MAX_PATH);
    std::wstring dir(self);
    const size_t sep = dir.find_last_of(L"\\/");
    if (sep != std::wstring::npos) dir.erase(sep + 1);
    else                           dir.clear();

    const std::wstring portable = dir + L"WorkAreaFullscreen.yaml";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable;

    wchar_t appdata[MAX_PATH]{};
    n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring installed = std::wstring(appdata) + L"\\WorkAreaFullscreen\\config.yaml";
        if (GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES) return installed;
    }

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"WorkAreaFullscreen.yaml";
}

bool ReadAllUtf8(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 1u << 20) { CloseHandle(h); return false; }

    out.resize(size);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, &out[0], size, &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

std::string NarrowA(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

DWORD WINAPI InitThread(LPVOID param)
{
    HkLogOpen();

    const std::wstring exeName = CurrentExeName();
    const std::wstring cfgPath = FindConfig(static_cast<HMODULE>(param));
    HkLog("target process: %s", NarrowA(exeName).c_str());
    HkLog("config: %s", NarrowA(cfgPath).c_str());

    std::string text;
    if (!ReadAllUtf8(cfgPath, text)) {
        HkLog("config missing or unreadable -> passthrough");
        return 0;
    }

    waf::AppConfig cfg;
    std::string    err;
    if (!waf::AppConfig::Parse(text, cfg, err)) {
        HkLog("config parse error: %s -> passthrough", err.c_str());
        return 0;
    }
    if (!cfg.enabled) {
        HkLog("config disabled globally -> passthrough");
        return 0;
    }

    const waf::TargetRule* rule = cfg.FindRule(NarrowA(exeName));
    if (!rule) {
        HkLog("no enabled rule matches this process -> passthrough");
        return 0;
    }

    HkEnv env;
    env.targetMod = GetModuleHandleW(nullptr);
    env.strategy  = rule->strategy;
    env.geometry  = rule->geometry;
    HkLog("rule matched: strategy=%s geometry=%s", env.strategy.c_str(), env.geometry.c_str());

    const MH_STATUS ms = MH_Initialize();
    HkLog("MH_Initialize -> %d", (int)ms);
    if (ms != MH_OK) return 0;

    int installed = 0;
    for (const waf::HkComponent* c : waf::HkRegistry()) {
        const waf::HookSetting* hs = nullptr;
        for (const auto& h : rule->hooks)
            if (h.name == c->name) { hs = &h; break; }

        if (!hs)            { HkLog("%s: not configured -> skip", c->name); continue; }
        if (!hs->enabled)   { HkLog("%s: disabled", c->name); continue; }
        if (!c->configure(*hs, env)) {
            HkLog("%s: configure rejected -> passthrough", c->name);
            continue;
        }
        if (rule->verifyCallsite && !c->verify(env)) {
            HkLog("%s: callsite verify FAILED -> passthrough", c->name);
            continue;
        }

        const MH_STATUS s = MH_CreateHookApi(c->dll, c->proc, c->detour, c->trampoline);
        if (s != MH_OK) {
            HkLog("%s: MH_CreateHookApi -> %d -> passthrough", c->name, (int)s);
            continue;
        }
        HkLog("%s: hook created", c->name);
        ++installed;
    }

    // 配置里写了但未注册的组件名：记录，便于扩展时发现拼写错误
    for (const auto& h : rule->hooks) {
        bool known = false;
        for (const waf::HkComponent* c : waf::HkRegistry())
            if (h.name == c->name) { known = true; break; }
        if (!known) HkLog("warning: unknown hook component '%s' (ignored)", h.name.c_str());
    }

    if (installed > 0) HkLog("MH_EnableHook -> %d", (int)MH_EnableHook(MH_ALL_HOOKS));
    HkLog("=== %d component(s) active ===", installed);
    return 0;
}

} // namespace
} // namespace waf

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // 注入发生在目标运行中，初始化放到独立线程，避免在 DllMain 内触发加载器锁问题。
        HANDLE t = CreateThread(nullptr, 0, waf::InitThread, hModule, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
