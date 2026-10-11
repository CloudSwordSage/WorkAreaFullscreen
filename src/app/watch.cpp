// watch.cpp - 目标进程扫描、后台延迟注入，以及命令行单次注入模式。
//
// 注入本身（OpenProcess / VirtualAllocEx / CreateRemoteThread / 等 LoadLibraryW）会阻塞
// 数百毫秒到数秒，目标若是正在启动的游戏还可能卡在加载器锁上，因此常驻模式下注入一律
// 交给后台线程，主线程只收结果，避免界面未响应。
#include "app.h"

#include <tlhelp32.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace waf::app {
namespace {

// 后台线程写、主线程取，用互斥量保护。
std::mutex                g_resultMutex;
std::vector<InjectResult> g_results;

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

        for (DWORD pid : FindProcessesByName(t.executable)) {
            const bool known =
                std::find(st.injectedPids.begin(), st.injectedPids.end(), pid) != st.injectedPids.end() ||
                std::find(st.injecting.begin(), st.injecting.end(), pid) != st.injecting.end() ||
                std::any_of(st.pending.begin(), st.pending.end(),
                            [pid](const PendingInject& p) { return p.pid == pid; });
            if (known) continue;

            PendingInject pj;
            pj.pid = pid;
            pj.exe = t.executable;
            pj.due = GetTickCount64() + static_cast<ULONGLONG>(st.config.injectDelayMs);
            st.pending.push_back(pj);
            Log(st, "发现 %s（PID %lu），%d ms 后注入",
                t.executable.c_str(), pid, st.config.injectDelayMs);
        }
    }
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
