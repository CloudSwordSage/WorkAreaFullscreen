// main.cpp - 入口：配置编辑器 + 启动代理管理；也承担提权后的 IFEO 写入。
#include "app/app.h"

#include <stdio.h>

#include <string>

#include "waf/ifeo.h"

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
    // 提权模式：ApplyLaunchProxy 用 runas 拉起本程序并把操作带过来，做完即退。
    if (argc >= 3) {
        const std::wstring first = argv[1];
        if (first == L"--ifeo-install" || first == L"--ifeo-uninstall") {
            const std::string exeName = waf::ExeNameOf(waf::app::Narrow(argv[2]));
            return waf::app::RunIfeoCommand(exeName, first == L"--ifeo-install");
        }
    }

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
