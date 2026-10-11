// hookkit.cpp - 组件式 API 返回值改写宿主（DLL）。
//
// 职责：读取配置 -> 匹配当前进程 -> 遍历组件注册表 -> 校验并安装 hook -> 放行目标。
// 组件本身不含任何配置解析之外的策略；行为差异全部来自 config/hookkit.yaml。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string>

#include "MinHook.h"
#include "hookkit/component.h"
#include "hookkit/runtime.h"
#include "hookkit/yamlmini.h"

namespace {

const wchar_t* kReadyEvent = L"HOOKKIT_READY";

std::wstring CurrentExeName()
{
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, path, MAX_PATH);
    const wchar_t* sep = wcsrchr(path, L'\\');
    return sep ? std::wstring(sep + 1) : std::wstring(path);
}

// 配置查找顺序：HOOKKIT_CONFIG -> 宿主 DLL 同目录\hookkit.yaml -> %TEMP%\hookkit.yaml。
std::wstring FindConfig(HMODULE selfMod)
{
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD   n             = GetEnvironmentVariableW(L"HOOKKIT_CONFIG", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;

    wchar_t self[MAX_PATH] = { 0 };
    GetModuleFileNameW(selfMod, self, MAX_PATH);
    std::wstring dir(self);
    const size_t sep = dir.find_last_of(L"\\/");
    if (sep != std::wstring::npos) dir.erase(sep + 1);
    else dir.clear();

    const std::wstring local = dir + L"hookkit.yaml";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;

    wchar_t tmp[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"hookkit.yaml";
}

bool ReadAllUtf8(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 1u << 20) { CloseHandle(h); return false; }

    out.resize(size);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, &out[0], size, &got, NULL);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

bool   BoolOf(const yamlmini::Node* n, bool def)          { return n ? n->toBool(def) : def; }
std::string StrOf(const yamlmini::Node* n, const char* def) { return n ? n->str(def) : std::string(def); }

// 在 targets 序列里选中与当前进程名一致且 enabled 的第一项。
const yamlmini::Node* SelectTarget(const yamlmini::Node* targets, const std::wstring& exeName)
{
    if (!targets || targets->kind != yamlmini::Node::Seq) return nullptr;

    const std::string me = Narrow(exeName);
    for (const auto& t : targets->seq) {
        if (!BoolOf(t.find("enabled"), true)) continue;
        std::string exe = StrOf(t.find("executable"), "");
        if (!exe.empty() && _stricmp(exe.c_str(), me.c_str()) == 0) return &t;
    }
    return nullptr;
}

void Finish(HANDLE ready)
{
    if (ready) SetEvent(ready);
}

DWORD WINAPI InitThread(LPVOID param)
{
    HkLogOpen();

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, kReadyEvent);

    const std::wstring cfgPath = FindConfig(static_cast<HMODULE>(param));
    HkLog("target process: %s", Narrow(CurrentExeName()).c_str());
    HkLog("config: %s", Narrow(cfgPath).c_str());

    std::string text;
    if (!ReadAllUtf8(cfgPath, text)) {
        HkLog("config missing or unreadable -> passthrough");
        Finish(ready);
        return 0;
    }

    yamlmini::Node root;
    std::string    err;
    if (!yamlmini::Parse(text, root, err)) {
        HkLog("config parse error: %s -> passthrough", err.c_str());
        Finish(ready);
        return 0;
    }

    if (!BoolOf(root.find("enabled"), true)) {
        HkLog("config disabled globally -> passthrough");
        Finish(ready);
        return 0;
    }

    const yamlmini::Node* target = SelectTarget(root.find("targets"), CurrentExeName());
    if (!target) {
        HkLog("no enabled target matches this process -> passthrough");
        Finish(ready);
        return 0;
    }

    HkEnv env;
    env.targetMod = GetModuleHandleW(NULL);
    env.strategy  = StrOf(target->find("strategy"), "sdl2-gdi");
    env.geometry  = StrOf(target->find("geometry"), "work_area");
    HkLog("target matched: strategy=%s geometry=%s", env.strategy.c_str(), env.geometry.c_str());

    const yamlmini::Node* safety   = target->find("safety");
    const std::string     fallback = StrOf(safety ? safety->find("fallback") : nullptr, "passthrough");
    const bool            verify   = BoolOf(safety ? safety->find("verify_callsite") : nullptr, true);
    HkLog("safety: fallback=%s verify_callsite=%d", fallback.c_str(), (int)verify);

    const yamlmini::Node* hooks = target->find("hooks");

    // 配置里写了但未注册的组件名：记录，便于扩展时发现拼写错误。
    if (hooks && hooks->kind == yamlmini::Node::Map) {
        for (const auto& kv : hooks->map) {
            bool known = false;
            for (const HkComponent* c : HkRegistry())
                if (kv.first == c->name) { known = true; break; }
            if (!known) HkLog("warning: unknown hook component '%s' (ignored)", kv.first.c_str());
        }
    }

    const MH_STATUS ms = MH_Initialize();
    HkLog("MH_Initialize -> %d", (int)ms);
    if (ms != MH_OK) { Finish(ready); return 0; }

    int installed = 0;
    for (const HkComponent* c : HkRegistry()) {
        const yamlmini::Node* hn = hooks ? hooks->find(c->name) : nullptr;
        if (!hn) { HkLog("%s: not configured -> skip", c->name); continue; }
        if (!BoolOf(hn->find("enabled"), true)) { HkLog("%s: disabled", c->name); continue; }

        if (!c->configure(*hn, env)) {
            HkLog("%s: configure rejected -> %s", c->name, fallback.c_str());
            continue;
        }
        if (verify && !c->verify(env)) {
            HkLog("%s: callsite verify FAILED -> %s", c->name, fallback.c_str());
            continue;
        }

        const MH_STATUS s = MH_CreateHookApi(c->dll, c->proc, c->detour, c->trampoline);
        if (s != MH_OK) {
            HkLog("%s: MH_CreateHookApi -> %d -> %s", c->name, (int)s, fallback.c_str());
            continue;
        }
        HkLog("%s: hook created", c->name);
        ++installed;
    }

    if (installed > 0) HkLog("MH_EnableHook -> %d", (int)MH_EnableHook(MH_ALL_HOOKS));
    HkLog("=== %d component(s) active ===", installed);
    Finish(ready);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // 注入发生在目标运行中，初始化放到独立线程，避免在 DllMain 内触发加载器锁问题。
        HANDLE t = CreateThread(NULL, 0, InitThread, hModule, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
