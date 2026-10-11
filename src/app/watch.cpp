// watch.cpp - 目标进程扫描、延迟注入，以及命令行单次注入模式。
#include "app.h"

#include <tlhelp32.h>

#include <string>
#include <vector>

namespace waf::app {
namespace {

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

    bool ok = WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr) != FALSE;
    if (!ok) {
        err = "WriteProcessMemory 失败";
    } else {
        FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE  th      = CreateRemoteThread(proc, nullptr, 0,
                                             reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
                                             remote, 0, nullptr);
        if (!th) {
            err = "CreateRemoteThread 失败";
            ok  = false;
        } else {
            WaitForSingleObject(th, 15000);
            DWORD hmod = 0;
            GetExitCodeThread(th, &hmod);
            CloseHandle(th);
            if (hmod == 0) err = "目标进程内的 LoadLibraryW 返回空（路径或位数不符）";
        }
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
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
            bool known = false;
            for (DWORD p : st.injectedPids) if (p == pid) { known = true; break; }
            for (const auto& pj : st.pending) if (pj.pid == pid) { known = true; break; }
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
        if (now < it->due)       { ++it; continue; }
        if (!IsAlive(it->pid))   { it = st.pending.erase(it); continue; }

        std::string err;
        if (InjectDll(it->pid, dll, err)) {
            Log(st, "已注入 %s（PID %lu）", it->exe.c_str(), it->pid);
            st.injectedPids.push_back(it->pid);
        } else {
            Log(st, "注入 %s（PID %lu）失败：%s", it->exe.c_str(), it->pid, err.c_str());
        }
        it = st.pending.erase(it);
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
