// main.cpp - HookLauncher：以调试器身份启动目标，在目标执行用户代码之前注入 injector.dll。
//
// 这是“第二级”启动方式，用来彻底摆脱注入时机问题：目标进程在第一条用户指令之前就被挂住，
// hook 一定早于它自己的显示初始化（SDL 枚举显示器、缓存 desktop_mode）。
//
// 配合 IFEO（Image File Execution Options）使用，需要管理员权限写注册表：
//   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<target.exe>
//       Debugger = "<安装目录>\HookLauncher.exe"
// 之后任何程序启动 <target.exe>，Windows 都会先启动本程序并把目标路径作为第一个参数
// （IFEO 只是把调试器插到命令行前面），于是启动链变成：
//   任意程序 -> IFEO -> HookLauncher -> 目标（挂起）-> 注入 -> 恢复执行
//
// 也支持直接调用：HookLauncher.exe <target.exe> [目标参数...]
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <string.h>

namespace {

HANDLE g_log = INVALID_HANDLE_VALUE;

void LogOpen()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"WorkAreaFullscreen-launcher.log";
    g_log = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (g_log == INVALID_HANDLE_VALUE) return;
    char line[1200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%6llu ms] %s\r\n",
                static_cast<unsigned long long>(GetTickCount64()), body);
    DWORD wrote = 0;
    WriteFile(g_log, line, static_cast<DWORD>(strlen(line)), &wrote, nullptr);
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring SelfExePath()
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return self;
}

std::wstring DirOf(const std::wstring& path)
{
    const size_t sep = path.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? std::wstring(L".") : path.substr(0, sep);
}

std::wstring BaseName(const std::wstring& path)
{
    const size_t sep = path.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? path : path.substr(sep + 1);
}

/**
 * 注入 DLL。
 *
 * @param targetSuspended 目标是否正被调试器挂起。挂起时远程线程要等 ContinueDebugEvent 之后
 *                        才会执行，所以既不能等它结束，也不能回收 remote 缓冲区（目标稍后才会
 *                        读取其中的路径字符串）。
 */
bool InjectDll(DWORD pid, const std::wstring& dllPath, bool targetSuspended, std::string& err)
{
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE proc = OpenProcess(access, FALSE, pid);
    if (!proc) {
        err = "OpenProcess 失败 err=" + std::to_string(GetLastError());
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
            err = "CreateRemoteThread 失败 err=" + std::to_string(GetLastError());
        } else if (targetSuspended) {
            // 目标被挂起：远程线程尚未执行，交给 ContinueDebugEvent 放行即可，这里立即返回
            ok      = true;
            freeBuf = false;
        } else {
            const DWORD w = WaitForSingleObject(th, 30000);
            if (w == WAIT_OBJECT_0) {
                DWORD hmod = 0;
                GetExitCodeThread(th, &hmod);
                ok = (hmod != 0);
                if (!ok) err = "目标内的 LoadLibraryW 返回空";
            } else {
                err     = "远程线程未在 30 秒内结束";
                freeBuf = false;
            }
            CloseHandle(th);
        }
    }

    if (freeBuf) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

// ---- IFEO 注册表 ----

const wchar_t* kIfeoRoot = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

bool SetIfeo(const std::wstring& exeName, const std::wstring& debugger, std::string& err)
{
    const std::wstring sub = std::wstring(kIfeoRoot) + L"\\" + exeName;
    HKEY               key = nullptr;
    const LSTATUS      st  = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, nullptr,
                                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        err = "创建 IFEO 键失败（需要管理员权限）err=" + std::to_string(st);
        return false;
    }

    const DWORD   bytes = static_cast<DWORD>((debugger.size() + 1) * sizeof(wchar_t));
    const LSTATUS st2   = RegSetValueExW(key, L"Debugger", 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(debugger.c_str()), bytes);
    RegCloseKey(key);
    if (st2 != ERROR_SUCCESS) {
        err = "写入 Debugger 值失败 err=" + std::to_string(st2);
        return false;
    }
    return true;
}

bool ClearIfeo(const std::wstring& exeName, std::string& err)
{
    const std::wstring sub = std::wstring(kIfeoRoot) + L"\\" + exeName;
    HKEY               key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        err = "打开 IFEO 键失败（可能本就没安装过）";
        return false;
    }
    const LSTATUS st = RegDeleteValueW(key, L"Debugger");
    RegCloseKey(key);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        err = "删除 Debugger 值失败 err=" + std::to_string(st);
        return false;
    }
    return true;
}

// ---- 作为调试器运行目标 ----

int RunAsDebugger(const std::wstring& target, const std::wstring& args)
{
    const std::wstring dll = DirOf(SelfExePath()) + L"\\injector.dll";

    std::wstring cmd = L"\"" + target + L"\"";
    if (!args.empty()) cmd += L" " + args;

    STARTUPINFOW        si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // DEBUG_ONLY_THIS_PROCESS 有两个作用：
    //   1) 目标主线程停在系统断点，尚未执行任何用户代码，这正是我们要的注入窗口；
    //   2) 带 DEBUG 标志的 CreateProcess 不会再走一遍 IFEO，避免自我递归。
    if (!CreateProcessW(target.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        DEBUG_ONLY_THIS_PROCESS, nullptr, DirOf(target).c_str(), &si, &pi)) {
        Log("CreateProcess 失败 err=%lu", GetLastError());
        return 2;
    }
    Log("已作为调试器启动 %s（PID %lu），准备注入 %s",
        Narrow(target).c_str(), pi.dwProcessId, Narrow(dll).c_str());

    bool   injected = false;
    int    retries  = 0;
    bool   firstBreakpoint = true;
    DEBUG_EVENT ev{};

    for (;;) {
        if (!WaitForDebugEvent(&ev, INFINITE)) break;

        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT: {
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            std::string err;
            // 目标此刻停在系统断点、尚未执行任何用户代码，是注入的最佳时机
            if (InjectDll(pi.dwProcessId, dll, /*targetSuspended=*/true, err)) {
                injected = true;
                Log("已在目标执行用户代码前完成注入（DLL 将随放行加载）");
            } else {
                Log("首次注入失败：%s（等加载器跑起来再重试）", err.c_str());
            }
            break;
        }

        case LOAD_DLL_DEBUG_EVENT: {
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            if (!injected && retries < 8) {           // 仅在注入 API 本身失败时才重试
                ++retries;
                std::string err;
                if (InjectDll(pi.dwProcessId, dll, /*targetSuspended=*/false, err)) {
                    injected = true;
                    Log("注入成功（第 %d 次尝试）", retries);
                }
            }
            break;
        }

        case OUTPUT_DEBUG_STRING_EVENT: {
            // 不把调试字符串取走会让目标阻塞在 OutputDebugString 上
            const DWORD len = ev.u.DebugString.nDebugStringLength;
            if (len > 0) {
                const SIZE_T bytes = len * (ev.u.DebugString.fUnicode ? sizeof(WCHAR) : 1);
                std::string  buf(bytes, '\0');
                SIZE_T       got = 0;
                ReadProcessMemory(pi.hProcess, ev.u.DebugString.lpDebugStringData, &buf[0], bytes, &got);
            }
            break;
        }

        case EXCEPTION_DEBUG_EVENT: {
            const bool isBreakpoint = (ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT);
            if (isBreakpoint && firstBreakpoint) {
                firstBreakpoint = false;              // 调试器机制自带的初始断点，直接放行
                cont = DBG_CONTINUE;
            } else {
                cont = DBG_EXCEPTION_NOT_HANDLED;     // 其余异常交回目标自己的处理
            }
            break;
        }

        default:
            break;
        }

        const bool  exited = (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT);
        const DWORD code   = exited ? ev.u.ExitProcess.dwExitCode : 0;
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
        if (exited) {
            Log("目标已退出 code=%lu（注入%s）", code, injected ? "成功" : "未成功");
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return static_cast<int>(code);
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    LogOpen();

    if (argc < 2) {
        fwprintf(stderr,
                 L"usage:\n"
                 L"  HookLauncher.exe <target.exe> [args...]   作为启动代理启动目标并注入\n"
                 L"  HookLauncher.exe --install <target.exe>    安装 IFEO 启动代理（需管理员）\n"
                 L"  HookLauncher.exe --uninstall <target.exe>  卸载 IFEO 启动代理（需管理员）\n");
        return 2;
    }

    const std::wstring first = argv[1];

    if (first == L"--install" || first == L"--uninstall") {
        if (argc < 3) {
            fwprintf(stderr, L"缺少目标 exe 路径\n");
            return 2;
        }
        const std::wstring exeName = BaseName(argv[2]);

        std::string err;
        if (first == L"--install") {
            const std::wstring self = SelfExePath();
            if (!SetIfeo(exeName, L"\"" + self + L"\"", err)) {
                printf("安装失败：%s\n", err.c_str());
                return 1;
            }
            printf("已安装 IFEO 启动代理：%ls -> %ls\n", exeName.c_str(), self.c_str());
        } else {
            if (!ClearIfeo(exeName, err)) {
                printf("卸载失败：%s\n", err.c_str());
                return 1;
            }
            printf("已卸载 IFEO 启动代理：%ls\n", exeName.c_str());
        }
        return 0;
    }

    // 作为启动代理：argv[1] 是目标，其余原样透传给目标
    std::wstring args;
    for (int i = 2; i < argc; ++i) {
        if (i > 2) args += L' ';
        args += L'"';
        args += argv[i];
        args += L'"';
    }
    return RunAsDebugger(first, args);
}
