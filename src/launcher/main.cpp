// main.cpp - HookLauncher：以调试器身份启动目标，在目标执行用户代码之前注入
// injector.dll。
//
// 这是“第二级”启动方式，用来彻底摆脱注入时机问题：目标进程在第一条用户指令之前就被挂住，
// hook 一定早于它自己的显示初始化（SDL 枚举显示器、缓存 desktop_mode）。
//
// 配合 IFEO（Image File Execution Options）使用，需要管理员权限写注册表：
//   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution
//   Options\<target.exe>
//       Debugger = "<安装目录>\HookLauncher.exe"
// 之后任何程序启动 <target.exe>，Windows
// 都会先启动本程序并把目标路径作为第一个参数 （IFEO
// 只是把调试器插到命令行前面），于是启动链变成：
//   任意程序 -> IFEO -> HookLauncher -> 目标（挂起）-> 注入 -> 恢复执行
//
// 注入完成后主动解除调试关系（DebugActiveProcessStop）并让 HookLauncher 退出；
// 退出前用 DebugSetProcessKillOnExit(FALSE)
// 关掉“调试器退出即杀目标”的默认行为。
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

#include "waf/config.h"
#include "waf/config_io.h"
#include "waf/ifeo.h"

namespace {

    HANDLE g_log = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION g_logCs;
    bool g_logCsReady = false;

    constexpr int kDefaultInjectDelayMs = 400;

    // 调试循环的轮询间隔：WaitForDebugEvent 不能同时等待“注入完成”，
    // 只能按超时轮询；100ms 兼顾脱离的及时性与空闲时的空转开销。
    constexpr DWORD kPollMs = 100;

    // 注入线程完成后置位（InterlockedExchange），调试循环轮询后脱离调试。
    volatile LONG g_injectDone = 0;

    void LogOpen() {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring path =
            std::wstring(tmp) + L"WorkAreaFullscreen-launcher.log";
        g_log = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        InitializeCriticalSection(&g_logCs);
        g_logCsReady = true;
    }

    void Log(const char * fmt, ...) {
        char body[1024];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
        va_end(ap);

        if (g_log == INVALID_HANDLE_VALUE)
            return;

        char line[1200];
        _snprintf_s(
            line,
            sizeof(line),
            _TRUNCATE,
            "[%6llu ms] %s\r\n",
            static_cast<unsigned long long>(GetTickCount64()),
            body
        );

        DWORD wrote = 0;
        // 注入计时线程与调试事件循环都会写日志
        if (g_logCsReady)
            EnterCriticalSection(&g_logCs);
        WriteFile(
            g_log,
            line,
            static_cast<DWORD>(strlen(line)),
            &wrote,
            nullptr
        );
        if (g_logCsReady)
            LeaveCriticalSection(&g_logCs);
    }

    std::string Narrow(const std::wstring & w) {
        if (w.empty())
            return std::string();
        const int n = WideCharToMultiByte(
            CP_UTF8,
            0,
            w.c_str(),
            (int)w.size(),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        std::string s(static_cast<size_t>(n), '\0');
        if (n > 0)
            WideCharToMultiByte(
                CP_UTF8,
                0,
                w.c_str(),
                (int)w.size(),
                &s[0],
                n,
                nullptr,
                nullptr
            );
        return s;
    }

    std::wstring SelfExePath() {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        return self;
    }

    std::wstring DirOf(const std::wstring & path) {
        const size_t sep = path.find_last_of(L"\\/");
        return (sep == std::wstring::npos) ? std::wstring(L".")
                                           : path.substr(0, sep);
    }

    /**
     * 注入 DLL。
     *
     * 目标此时已处于正常（未挂起）运行状态，远程线程会立即执行，因此这里等待
     * LoadLibraryW 结束后才回收 remote 缓冲区。
     */
    bool InjectDll(DWORD pid, const std::wstring & dllPath, std::string & err) {
        const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                             PROCESS_VM_READ;
        HANDLE proc = OpenProcess(access, FALSE, pid);
        if (!proc) {
            err = "OpenProcess 失败 err=" + std::to_string(GetLastError());
            return false;
        }

        const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        LPVOID remote = VirtualAllocEx(
            proc,
            nullptr,
            bytes,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE
        );
        if (!remote) {
            CloseHandle(proc);
            err = "VirtualAllocEx 失败";
            return false;
        }

        bool ok = false;
        bool freeBuf = true;

        if (!WriteProcessMemory(
                proc,
                remote,
                dllPath.c_str(),
                bytes,
                nullptr
            )) {
            err = "WriteProcessMemory 失败";
        } else {
            FARPROC loadLib = GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"),
                "LoadLibraryW"
            );
            HANDLE th = CreateRemoteThread(
                proc,
                nullptr,
                0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
                remote,
                0,
                nullptr
            );
            if (!th) {
                err = "CreateRemoteThread 失败 err=" +
                      std::to_string(GetLastError());
            } else {
                const DWORD w = WaitForSingleObject(th, 30000);
                if (w == WAIT_OBJECT_0) {
                    DWORD hmod = 0;
                    GetExitCodeThread(th, &hmod);
                    ok = (hmod != 0);
                    if (!ok)
                        err = "目标内的 LoadLibraryW 返回空";
                } else {
                    err = "远程线程未在 30 秒内结束";
                    freeBuf = false;
                }
                CloseHandle(th);
            }
        }

        if (freeBuf)
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return ok;
    }

    // ---- 注入延迟（每条规则可配） ----

    /** 读配置取该目标要求的注入延迟；找不到配置或规则时用默认值。 */
    int InjectDelayFor(const std::wstring & targetPath) {
        const size_t sep = targetPath.find_last_of(L"\\/");
        const std::wstring base = (sep == std::wstring::npos)
                                      ? targetPath
                                      : targetPath.substr(sep + 1);
        const std::string exeName = Narrow(base);

        const std::wstring cfgPath = waf::FindConfigPath(DirOf(SelfExePath()));
        std::string text;
        if (!waf::ReadAllUtf8(cfgPath, text) || text.empty()) {
            Log("未读到配置（%s），注入延迟用默认 %d ms",
                Narrow(cfgPath).c_str(),
                kDefaultInjectDelayMs);
            return kDefaultInjectDelayMs;
        }

        waf::AppConfig cfg;
        std::string err;
        if (!waf::AppConfig::Parse(text, cfg, err)) {
            Log("配置解析失败：%s，注入延迟用默认 %d ms",
                err.c_str(),
                kDefaultInjectDelayMs);
            return kDefaultInjectDelayMs;
        }

        const waf::TargetRule * rule = cfg.FindRule(exeName);
        if (!rule) {
            Log("配置里没有 %s 的规则，注入延迟用默认 %d ms",
                exeName.c_str(),
                kDefaultInjectDelayMs);
            return kDefaultInjectDelayMs;
        }
        return rule->injectDelayMs;
    }

    /** 延迟注入任务：目标放行并跑起来之后，到点再注入。 */
    struct InjectJob {
            DWORD pid;
            std::wstring dll;
            int delayMs;
    };

    DWORD WINAPI InjectThread(LPVOID param) {
        InjectJob * job = static_cast<InjectJob *>(param);
        if (job->delayMs > 0)
            Sleep(static_cast<DWORD>(job->delayMs));

        std::string err;
        // 此时目标已被放行、正在初始化，按普通远程线程方式注入
        const bool ok = InjectDll(job->pid, job->dll, err);
        if (ok)
            Log("已在目标启动后约 %d ms 完成注入", job->delayMs);
        else
            Log("注入失败：%s", err.c_str());

        // 无论注入成败都置位：注入一旦结束就没有继续调试目标的理由。
        InterlockedExchange(&g_injectDone, 1);

        delete job;
        return 0;
    }

    // ---- 作为调试器运行目标 ----

    int RunAsDebugger(const std::wstring & target, const std::wstring & args) {
        const std::wstring dll = DirOf(SelfExePath()) + L"\\injector.dll";

        std::wstring cmd = L"\"" + target + L"\"";
        if (!args.empty())
            cmd += L" " + args;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // DEBUG_ONLY_THIS_PROCESS 有两个作用：
        //   1)
        //   目标主线程停在系统断点，尚未执行任何用户代码，这正是我们要的注入窗口；
        //   2) 带 DEBUG 标志的 CreateProcess 不会再走一遍 IFEO，避免自我递归。
        if (!CreateProcessW(
                target.c_str(),
                cmd.data(),
                nullptr,
                nullptr,
                FALSE,
                DEBUG_ONLY_THIS_PROCESS,
                nullptr,
                DirOf(target).c_str(),
                &si,
                &pi
            )) {
            Log("CreateProcess 失败 err=%lu", GetLastError());
            return 2;
        }
        // 目标会先跑起来（放行初始断点后立刻开始初始化），delayMs 之后再注入。
        // 这个延迟是必须的：在目标尚未初始化时注入会让部分引擎（例如群星）直接启动失败。
        const int delayMs = InjectDelayFor(target);
        Log("已作为调试器启动 %s（PID %lu），将在其启动后 %d ms 注入 %s",
            Narrow(target).c_str(),
            pi.dwProcessId,
            delayMs,
            Narrow(dll).c_str());

        bool firstBreakpoint = true;
        bool detach = false;
        DEBUG_EVENT ev{};

        for (;;) {
            // WaitForDebugEvent 不能同时等待“注入完成”，故按小超时轮询：无事件
            // 可处理时醒来检查计时线程是否已注入完毕。脱离只在超时（没有未放行
            // 的调试事件）分支进行，否则目标会停在那个事件上。
            if (!WaitForDebugEvent(&ev, kPollMs)) {
                if (g_injectDone) {
                    detach = true;
                    break;
                }
                if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
                    break; // 目标已退出且不再产生事件
                continue;
            }

            DWORD cont = DBG_CONTINUE;

            switch (ev.dwDebugEventCode) {
                case CREATE_PROCESS_DEBUG_EVENT: {
                    if (ev.u.CreateProcessInfo.hFile)
                        CloseHandle(ev.u.CreateProcessInfo.hFile);
                    // 起一个计时线程：到点后按普通方式注入（那时目标已被放行、正在初始化）
                    InjectJob * job =
                        new InjectJob{pi.dwProcessId, dll, delayMs};
                    HANDLE th =
                        CreateThread(nullptr, 0, InjectThread, job, 0, nullptr);
                    if (th)
                        CloseHandle(th);
                    else {
                        delete job;
                        Log("无法创建注入计时线程");
                    }
                    break;
                }

                case LOAD_DLL_DEBUG_EVENT: {
                    if (ev.u.LoadDll.hFile)
                        CloseHandle(ev.u.LoadDll.hFile);
                    break;
                }

                case OUTPUT_DEBUG_STRING_EVENT: {
                    // 不把调试字符串取走会让目标阻塞在 OutputDebugString 上
                    const DWORD len = ev.u.DebugString.nDebugStringLength;
                    if (len > 0) {
                        const SIZE_T bytes =
                            len *
                            (ev.u.DebugString.fUnicode ? sizeof(WCHAR) : 1);
                        std::string buf(bytes, '\0');
                        SIZE_T got = 0;
                        ReadProcessMemory(
                            pi.hProcess,
                            ev.u.DebugString.lpDebugStringData,
                            &buf[0],
                            bytes,
                            &got
                        );
                    }
                    break;
                }

                case EXCEPTION_DEBUG_EVENT: {
                    const bool isBreakpoint =
                        (ev.u.Exception.ExceptionRecord.ExceptionCode ==
                         EXCEPTION_BREAKPOINT);
                    if (isBreakpoint && firstBreakpoint) {
                        firstBreakpoint =
                            false; // 调试器机制自带的初始断点，直接放行
                        cont = DBG_CONTINUE;
                    } else {
                        cont =
                            DBG_EXCEPTION_NOT_HANDLED; // 其余异常交回目标自己的处理
                    }
                    break;
                }

                default:
                    break;
            }

            const bool exited =
                (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT);
            const DWORD code = exited ? ev.u.ExitProcess.dwExitCode : 0;
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
            if (exited) {
                Log("目标已退出 code=%lu", code);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return static_cast<int>(code);
            }
        }

        if (detach) {
            // 注入已结束：解除调试关系，让目标独立运行。DebugSetProcessKillOnExit
            // 必须在建立该调试连接的线程（本线程）调用，否则调试线程/进程退出时
            // 默认会把目标一并杀掉。
            DebugSetProcessKillOnExit(FALSE);
            if (DebugActiveProcessStop(pi.dwProcessId))
                Log("目标已脱离调试，继续独立运行（PID %lu）", pi.dwProcessId);
            else
                Log("DebugActiveProcessStop 失败 err=%lu", GetLastError());
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }

} // namespace

int wmain(int argc, wchar_t ** argv) {
    LogOpen();

    if (argc < 2) {
        fwprintf(
            stderr,
            L"usage:\n"
            L"  HookLauncher.exe <target.exe> [args...]   "
            L"作为启动代理启动目标并注入\n"
            L"  HookLauncher.exe --install <target.exe>    安装 IFEO "
            L"启动代理（需管理员）\n"
            L"  HookLauncher.exe --uninstall <target.exe>  卸载 IFEO "
            L"启动代理（需管理员）\n"
        );
        return 2;
    }

    const std::wstring first = argv[1];

    if (first == L"--install" || first == L"--uninstall") {
        if (argc < 3) {
            fwprintf(stderr, L"缺少目标 exe 路径\n");
            return 2;
        }
        const std::string exeName = waf::ExeNameOf(Narrow(argv[2]));

        std::string err;
        if (first == L"--install") {
            const std::wstring self = SelfExePath();
            if (!waf::IfeoInstall(exeName, L"\"" + self + L"\"", err)) {
                printf("安装失败：%s\n", err.c_str());
                return 1;
            }
            printf(
                "已安装 IFEO 启动代理：%s -> %ls\n",
                exeName.c_str(),
                self.c_str()
            );
        } else {
            if (!waf::IfeoUninstall(exeName, err)) {
                printf("卸载失败：%s\n", err.c_str());
                return 1;
            }
            printf("已卸载 IFEO 启动代理：%s\n", exeName.c_str());
        }
        return 0;
    }

    // 作为启动代理：argv[1] 是目标，其余原样透传给目标
    std::wstring args;
    for (int i = 2; i < argc; ++i) {
        if (i > 2)
            args += L' ';
        args += L'"';
        args += argv[i];
        args += L'"';
    }
    return RunAsDebugger(first, args);
}
