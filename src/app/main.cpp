// main.cpp - 入口：默认进入常驻 UI 模式，--inject 进入单次注入模式。
#include "app.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>

namespace {

std::wstring DefaultConfigPath()
{
    wchar_t appdata[MAX_PATH]{};
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
    std::wstring injectExe;
    int          delayOverride = -1;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool         hasNext = (i + 1 < argc);
        if (a == L"--config" && hasNext)     configPath    = argv[++i];
        else if (a == L"--inject" && hasNext) injectExe    = argv[++i];
        else if (a == L"--delay" && hasNext)  delayOverride = _wtoi(argv[++i]);
        else {
            printf("usage: WorkAreaFullscreen.exe [--config <path>] [--inject <exe>] [--delay <ms>]\n"
                   "  (无参数)         常驻：窗口 + 托盘，监听目标进程启动\n"
                   "  --inject <exe>   单次：启动该程序并按配置注入，等它退出\n"
                   "  --delay <ms>     覆盖配置里的注入延迟\n");
            return 2;
        }
    }

    if (configPath.empty()) configPath = DefaultConfigPath();
    configPath = ToAbsolute(configPath);
    waf::app::EnsureConfigFile(configPath);

    if (!injectExe.empty())
        return waf::app::RunInjectOnce(configPath, ToAbsolute(injectExe), delayOverride);

    FreeConsole();   // 常驻模式不需要控制台，避免多一个黑窗口
    return waf::app::RunUi(configPath);
}
