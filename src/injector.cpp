// injector.cpp - 按配置启动目标进程并注入 hookkit.dll。
//
// 目标与 DLL 位置来自配置；可被命令行开关覆盖。
// 目标 exe 相对当前工作目录解析（与 trace 文档的复现方式一致：工作目录 = 游戏目录）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string>

#include "hookkit/yamlmini.h"

namespace {

void Die(const char* what)
{
    printf("FAIL: %s (err=%lu)\n", what, GetLastError());
    ExitProcess(1);
}

bool ReadAll(const wchar_t* path, std::string& out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
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

// 取配置里第一个 enabled target 的 executable。
std::wstring TargetFromConfig(const std::wstring& cfgPath)
{
    std::string text;
    if (!ReadAll(cfgPath.c_str(), text)) {
        printf("FAIL: cannot read config\n");
        ExitProcess(2);
    }

    yamlmini::Node root;
    std::string    err;
    if (!yamlmini::Parse(text, root, err)) {
        printf("FAIL: config parse error: %s\n", err.c_str());
        ExitProcess(2);
    }

    const yamlmini::Node* targets = root.find("targets");
    if (targets && targets->kind == yamlmini::Node::Seq) {
        for (const auto& t : targets->seq) {
            const yamlmini::Node* en = t.find("enabled");
            if (en && !en->toBool(true)) continue;
            const yamlmini::Node* exe = t.find("executable");
            if (exe) {
                const std::string s = exe->str();
                if (!s.empty()) {
                    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
                    std::wstring w(static_cast<size_t>(n), L'\0');
                    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
                    return w;
                }
            }
        }
    }
    printf("FAIL: config has no enabled target executable\n");
    ExitProcess(2);
}

std::wstring DirOf(const std::wstring& path)
{
    const size_t sep = path.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? std::wstring(L".") : path.substr(0, sep);
}

std::wstring SelfDir()
{
    wchar_t self[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, self, MAX_PATH);
    return DirOf(self);
}

std::wstring ToAbsolute(const std::wstring& path)
{
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD   n             = GetFullPathNameW(path.c_str(), MAX_PATH, buf, NULL);
    if (n == 0 || n >= MAX_PATH) return path;
    return buf;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        printf("usage: injector.exe <config.yaml> [--exe <path>] [--dll <path>]\n"
               "                                      [--delay <ms>] [--run <ms>]\n");
        return 2;
    }

    const std::wstring cfgPath = ToAbsolute(argv[1]);
    std::wstring       exe, dll;
    DWORD              delay = 400;
    DWORD              runMs = 0;   // 0 = 等到目标退出

    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool         hasNext = (i + 1 < argc);
        if (a == L"--exe" && hasNext)        exe   = argv[++i];
        else if (a == L"--dll" && hasNext)   dll   = argv[++i];
        else if (a == L"--delay" && hasNext) delay = (DWORD)_wtoi(argv[++i]);
        else if (a == L"--run" && hasNext)   runMs = (DWORD)_wtoi(argv[++i]);
        else { printf("unknown option: %ls\n", a.c_str()); return 2; }
    }

    if (exe.empty()) exe = TargetFromConfig(cfgPath);
    exe = ToAbsolute(exe);
    if (dll.empty()) dll = SelfDir() + L"\\hookkit.dll";
    dll = ToAbsolute(dll);

    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: target not found: %ls\n", exe.c_str());
        return 2;
    }
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: dll not found: %ls\n", dll.c_str());
        return 2;
    }

    const std::wstring workDir = DirOf(exe);

    HANDLE ready = CreateEventW(NULL, FALSE, FALSE, L"HOOKKIT_READY");
    if (!ready) Die("CreateEvent");

    wchar_t cmd[MAX_PATH * 3];
    _snwprintf_s(cmd, _TRUNCATE, L"\"%s\"", exe.c_str());

    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    // 该游戏在 CREATE_SUSPENDED 下注入会立即崩溃（0xC0000005），故正常启动后再注入。
    if (!CreateProcessW(exe.c_str(), cmd, NULL, NULL, FALSE, 0, NULL, workDir.c_str(), &si, &pi))
        Die("CreateProcess");

    printf("target pid=%lu  exe=%ls\n", pi.dwProcessId, exe.c_str());
    printf("dll=%ls  delay=%lu ms  run=%lu ms\n", dll.c_str(), delay, runMs);
    fflush(stdout);

    if (delay) Sleep(delay);

    const SIZE_T bytes  = (dll.size() + 1) * sizeof(wchar_t);
    LPVOID       remote = VirtualAllocEx(pi.hProcess, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) Die("VirtualAllocEx");
    if (!WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, NULL)) Die("WriteProcessMemory");

    FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!loadLib) Die("GetProcAddress(LoadLibraryW)");

    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!th) Die("CreateRemoteThread");
    WaitForSingleObject(th, 15000);

    DWORD hmod = 0;
    GetExitCodeThread(th, &hmod);
    CloseHandle(th);
    printf("remote LoadLibraryW hmod=0x%08lX\n", hmod);

    const DWORD w = WaitForSingleObject(ready, 15000);
    printf("ready event: %s\n", (w == WAIT_OBJECT_0) ? "signaled" : "TIMEOUT");
    fflush(stdout);

    if (runMs == 0) {
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else if (WaitForSingleObject(pi.hProcess, runMs) == WAIT_TIMEOUT) {
        printf("run window elapsed, terminating\n");
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    printf("target exited, code=%lu\n", ec);
    return 0;
}
