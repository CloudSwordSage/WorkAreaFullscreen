// main.cpp - 入口：配置编辑器 + 启动代理管理者。
//
// 实际注入由 HookLauncher.exe 在目标执行第一条指令之前完成，本程序不监听进程、不注入。
#include "app.h"

#include <stdio.h>

#include <string>

namespace {

std::wstring DefaultConfigPath()
{
    wchar_t     appdata[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring dir = std::wstring(appdata) + L"\\WorkAreaFullscreen";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\config.yaml";
    }
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"WorkAreaFullscreen.yaml";
}

std::wstring ToAbsolute(const std::wstring& path)
{
    wchar_t buf[MAX_PATH]{};
    if (GetFullPathNameW(path.c_str(), MAX_PATH, buf, nullptr) == 0) return path;
    return buf;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring configPath;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else {
            printf("usage: WorkAreaFullscreen.exe [--config <path>]\n"
                   "  配置编辑器 + 启动代理管理者。\n"
                   "  实际注入由同目录的 HookLauncher.exe 在目标启动时完成。\n");
            return 2;
        }
    }

    if (configPath.empty()) configPath = DefaultConfigPath();
    configPath = ToAbsolute(configPath);
    waf::app::EnsureConfigFile(configPath);

    FreeConsole();   // 界面程序不需要控制台
    return waf::app::RunUi(configPath);
}
