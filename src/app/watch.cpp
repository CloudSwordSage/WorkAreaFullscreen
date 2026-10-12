// watch.cpp - 目标进程发现（WMI 进程创建事件，轮询作回退）、后台延迟注入，以及单次注入模式。
//
// 注入本身（OpenProcess / VirtualAllocEx / CreateRemoteThread / 等 LoadLibraryW）会阻塞
// 数百毫秒到数秒，目标若是正在启动的游戏还可能卡在加载器锁上，因此常驻模式下注入一律
// 交给后台线程，主线程只收结果，避免界面未响应。
#include "app.h"

#include <tlhelp32.h>
#include <wbemidl.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace waf::app {
namespace {

// 后台线程写、主线程取，用互斥量保护。
std::mutex                g_resultMutex;
std::vector<InjectResult> g_results;

// ---- WMI 进程创建事件 ----
//
// Win32_ProcessStartTrace 是内核 ETW 事件在 WMI 里的投影：目标进程一创建就回调，不必轮询，
// 也就不会像固定间隔扫描那样“晚发现”而错过目标的显示初始化窗口（实测错过时晚到 961 ms）。
// 该事件需要管理员权限；订阅失败时退回轮询扫描。
struct ProcessStartEvent {
    DWORD       pid = 0;
    std::string exe;
};

std::mutex                     g_eventMutex;
std::vector<ProcessStartEvent> g_events;        // 订阅线程写、主线程取
std::vector<std::string>       g_watcherNotes;  // 订阅状态提示，交主线程记日志
std::atomic<bool>              g_watcherReady{false};

DWORD WINAPI WmiWatcherThread(LPVOID)
{
    auto note = [](const char* text) {
        std::lock_guard<std::mutex> lk(g_eventMutex);
        g_watcherNotes.push_back(text);
    };

    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) {
        note("WMI：CoInitializeEx 失败，回退轮询扫描");
        return 0;
    }
    // 同进程只能设置一次；已被别处设置过会返回 RPC_E_TOO_LATE，忽略即可。
    // 认证级别必须是 PacketPrivacy：Win32_ProcessStartTrace 属于内核 ETW 事件，
    // 较低的 CALL/Default 级别会被拒绝（实测同一台机器上 PowerShell 能订阅、这里失败）。
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, reinterpret_cast<void**>(&locator))) || !locator) {
        note("WMI：CoCreateInstance 失败，回退轮询扫描");
        return 0;
    }

    BSTR           ns       = SysAllocString(L"ROOT\\CIMV2");
    IWbemServices* services = nullptr;
    const HRESULT  hrConnect = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0,
                                                      nullptr, nullptr, &services);
    SysFreeString(ns);
    locator->Release();
    if (FAILED(hrConnect) || !services) {
        note("WMI：连接 ROOT\\CIMV2 失败，回退轮询扫描");
        return 0;
    }

    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    BSTR                  lang   = SysAllocString(L"WQL");
    BSTR                  query  = SysAllocString(L"SELECT * FROM Win32_ProcessStartTrace");
    IEnumWbemClassObject* events = nullptr;
    const HRESULT         hrSub  = services->ExecNotificationQuery(
        lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &events);
    SysFreeString(lang);
    SysFreeString(query);
    services->Release();

    if (FAILED(hrSub) || !events) {
        if (hrSub == WBEM_E_ACCESS_DENIED) {
            note("WMI：订阅进程创建事件被拒绝（该事件需要管理员权限），回退轮询扫描；"
                 "以管理员身份运行可获得零延迟的进程发现");
        } else {
            char buf[192];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "WMI：订阅 Win32_ProcessStartTrace 失败（hr=0x%08lX），回退轮询扫描",
                        static_cast<unsigned>(hrSub));
            note(buf);
        }
        return 0;
    }

    g_watcherReady = true;
    note("已订阅 Win32_ProcessStartTrace 进程创建事件");

    for (;;) {
        IWbemClassObject* obj = nullptr;
        ULONG             got = 0;
        if (FAILED(events->Next(WBEM_INFINITE, 1, &obj, &got)) || got == 0) break;

        ProcessStartEvent ev;
        VARIANT           vName, vPid;
        VariantInit(&vName);
        VariantInit(&vPid);
        if (SUCCEEDED(obj->Get(L"ProcessName", 0, &vName, nullptr, nullptr)) && vName.vt == VT_BSTR)
            ev.exe = Narrow(std::wstring(vName.bstrVal));
        if (SUCCEEDED(obj->Get(L"ProcessID", 0, &vPid, nullptr, nullptr))) {
            if (vPid.vt == VT_I4)       ev.pid = static_cast<DWORD>(vPid.lVal);
            else if (vPid.vt == VT_UI4) ev.pid = static_cast<DWORD>(vPid.ulVal);
        }
        VariantClear(&vName);
        VariantClear(&vPid);
        obj->Release();

        if (ev.pid == 0 || ev.exe.empty()) continue;
        std::lock_guard<std::mutex> lk(g_eventMutex);
        g_events.push_back(std::move(ev));
    }

    events->Release();
    g_watcherReady = false;
    note("WMI 事件订阅已结束，回退轮询扫描");
    return 0;
}

std::vector<DWORD> FindProcessesByName(const std::string& name)
{
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const std::string exe = Narrow(std::wstring(pe.szExeFile));
            if (_stricmp(exe.c_str(), name.c_str()) == 0) out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

bool IsAlive(DWORD pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) != WAIT_OBJECT_0;
    CloseHandle(h);
    return alive;
}

/** 当前时刻的 FILETIME 毫秒值（与 GetProcessTimes 的创建时间同基准）。 */
ULONGLONG NowFileTimeMs()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000ull;
}

/** 进程创建时刻换算成 FILETIME 毫秒；取不到返回 false。 */
bool ProcessStartFileTimeMs(DWORD pid, ULONGLONG& out)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;

    FILETIME create{}, exitTime{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(h, &create, &exitTime, &kernel, &user);
    CloseHandle(h);
    if (!ok) return false;

    ULARGE_INTEGER u;
    u.LowPart  = create.dwLowDateTime;
    u.HighPart = create.dwHighDateTime;
    out = u.QuadPart / 10000ull;
    return true;
}

bool InjectDll(DWORD pid, const std::wstring& dllPath, std::string& err)
{
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE proc = OpenProcess(access, FALSE, pid);
    if (!proc) {
        err = "OpenProcess 失败；若目标以管理员身份运行，本程序也需以管理员身份启动";
        return false;
    }

    const SIZE_T bytes  = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID       remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(proc);
        err = "VirtualAllocEx 失败";
        return false;
    }

    bool ok      = false;
    bool freeBuf = true;

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        err = "WriteProcessMemory 失败";
    } else {
        FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE  th      = CreateRemoteThread(proc, nullptr, 0,
                                             reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
                                             remote, 0, nullptr);
        if (!th) {
            err = "CreateRemoteThread 失败";
        } else {
            const DWORD w = WaitForSingleObject(th, 30000);
            if (w == WAIT_OBJECT_0) {
                DWORD hmod = 0;
                GetExitCodeThread(th, &hmod);
                ok = (hmod != 0);
                if (!ok) err = "目标进程内的 LoadLibraryW 返回空（路径或位数不符）";
            } else {
                // 目标迟迟不返回：不回收 remote，否则它仍在读取路径字符串时会崩掉目标。
                err     = "等待目标加载 DLL 超时（目标可能仍在初始化）";
                freeBuf = false;
            }
            CloseHandle(th);
        }
    }

    if (freeBuf) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

void RunInjectAsync(DWORD pid, const std::wstring& dllPath, const std::string& exe)
{
    std::thread([pid, dllPath, exe]() {
        std::string  err;
        InjectResult r;
        r.pid = pid;
        r.exe = exe;
        r.ok  = InjectDll(pid, dllPath, err);
        r.err = std::move(err);

        std::lock_guard<std::mutex> lk(g_resultMutex);
        g_results.push_back(std::move(r));
    }).detach();
}

/** 为一次“目标进程已创建”排定注入：匹配规则、按启动时刻算延迟、入队。 */
void QueueInjection(State& st, const std::string& exeName, DWORD pid)
{
    if (!st.config.enabled) return;
    if (!st.config.FindRule(exeName)) return;

    const bool known =
        std::find(st.injectedPids.begin(), st.injectedPids.end(), pid) != st.injectedPids.end() ||
        std::find(st.injecting.begin(), st.injecting.end(), pid) != st.injecting.end() ||
        std::any_of(st.pending.begin(), st.pending.end(),
                    [pid](const PendingInject& p) { return p.pid == pid; });
    if (known) return;

    // 延迟以“目标进程启动时刻”为基准，而不是“发现的时刻”：目标往往在显示初始化时就把
    // 分辨率等信息缓存好了，晚注入就再也改不到窗口尺寸。
    const ULONGLONG nowTick = GetTickCount64();
    ULONGLONG       elapsed = 0;
    const bool      haveStart = ProcessStartFileTimeMs(pid, elapsed);
    if (haveStart) {
        const ULONGLONG nowFt = NowFileTimeMs();
        elapsed = (nowFt > elapsed) ? (nowFt - elapsed) : 0;
    }

    LONGLONG remain = static_cast<LONGLONG>(st.config.injectDelayMs) -
                      static_cast<LONGLONG>(haveStart ? elapsed : 0);
    if (remain < 0) remain = 0;

    PendingInject pj;
    pj.pid = pid;
    pj.exe = exeName;
    pj.due = nowTick + static_cast<ULONGLONG>(remain);
    st.pending.push_back(pj);

    if (haveStart)
        Log(st, "发现 %s（PID %lu，已运行 %llu ms），将在其启动后 %d ms 注入",
            exeName.c_str(), pid, elapsed, st.config.injectDelayMs);
    else
        Log(st, "发现 %s（PID %lu），%d ms 后注入", exeName.c_str(), pid, st.config.injectDelayMs);
}

} // namespace

void WatchStep(State& st)
{
    // 已退出进程出队，避免列表无限增长
    for (auto it = st.injectedPids.begin(); it != st.injectedPids.end(); ) {
        if (IsAlive(*it)) ++it;
        else              it = st.injectedPids.erase(it);
    }
    for (auto it = st.pending.begin(); it != st.pending.end(); ) {
        if (IsAlive(it->pid)) ++it;
        else                  it = st.pending.erase(it);
    }

    for (const auto& t : st.config.targets) {
        if (!t.enabled || t.executable.empty()) continue;
        for (DWORD pid : FindProcessesByName(t.executable)) QueueInjection(st, t.executable, pid);
    }
}

void CollectProcessEvents(State& st)
{
    std::vector<ProcessStartEvent> events;
    std::vector<std::string>       notes;
    {
        std::lock_guard<std::mutex> lk(g_eventMutex);
        events.swap(g_events);
        notes.swap(g_watcherNotes);
    }
    for (const auto& n : notes) Log(st, "%s", n.c_str());
    for (const auto& e : events) QueueInjection(st, e.exe, e.pid);
}

bool ProcessWatcherActive()
{
    return g_watcherReady.load();
}

void StartProcessWatcher(State& st)
{
    HANDLE t = CreateThread(nullptr, 0, WmiWatcherThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void ProcessPendingInjections(State& st)
{
    if (st.pending.empty()) return;

    const std::wstring dll = SelfDir() + L"\\injector.dll";
    const ULONGLONG    now = GetTickCount64();

    for (auto it = st.pending.begin(); it != st.pending.end(); ) {
        if (now < it->due)     { ++it; continue; }
        if (!IsAlive(it->pid)) { it = st.pending.erase(it); continue; }

        const bool busy = std::find(st.injecting.begin(), st.injecting.end(), it->pid) != st.injecting.end();
        if (busy) { ++it; continue; }

        st.injecting.push_back(it->pid);
        Log(st, "开始注入 %s（PID %lu）", it->exe.c_str(), it->pid);
        RunInjectAsync(it->pid, dll, it->exe);
        it = st.pending.erase(it);
    }
}

void CollectInjectResults(State& st)
{
    std::vector<InjectResult> done;
    {
        std::lock_guard<std::mutex> lk(g_resultMutex);
        done.swap(g_results);
    }

    for (const auto& r : done) {
        st.injecting.erase(std::remove(st.injecting.begin(), st.injecting.end(), r.pid),
                           st.injecting.end());
        if (r.ok) {
            Log(st, "已注入 %s（PID %lu）", r.exe.c_str(), r.pid);
            st.injectedPids.push_back(r.pid);
        } else {
            Log(st, "注入 %s（PID %lu）失败：%s", r.exe.c_str(), r.pid, r.err.c_str());
        }
    }
}

int RunInjectOnce(const std::wstring& configPath, const std::wstring& exePath, int delayMsOverride)
{
    State st;
    st.configPath = configPath;
    OpenLogFile();
    ReloadConfig(st);

    if (GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(st, "目标不存在：%s", Narrow(exePath).c_str());
        return 2;
    }

    const std::wstring dll = SelfDir() + L"\\injector.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(st, "找不到 injector.dll：%s", Narrow(dll).c_str());
        return 2;
    }

    std::wstring cmd = L"\"" + exePath + L"\"";
    STARTUPINFOW        si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        DirOf(exePath).c_str(), &si, &pi)) {
        Log(st, "CreateProcess 失败（%lu）", GetLastError());
        return 2;
    }
    Log(st, "已启动 %s（PID %lu）", Narrow(exePath).c_str(), pi.dwProcessId);

    const int delay = (delayMsOverride >= 0) ? delayMsOverride : st.config.injectDelayMs;
    if (delay > 0) Sleep(delay);

    std::string err;
    if (InjectDll(pi.dwProcessId, dll, err)) Log(st, "注入成功");
    else                                     Log(st, "注入失败：%s", err.c_str());

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log(st, "目标已退出，code=%lu", ec);
    return static_cast<int>(ec);
}

} // namespace waf::app
