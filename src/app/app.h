// app.h - 常驻宿主的状态与模块接口。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>

#include "waf/config.h"

namespace waf::app {

/** 界面上的一条日志。 */
struct LogEntry {
    std::string time;   ///< HH:MM:SS
    std::string text;
};

/** 待注入的进程：发现后等待 injectDelayMs 再注入（立即注入会让部分引擎崩溃）。 */
struct PendingInject {
    DWORD       pid = 0;
    std::string exe;
    ULONGLONG   due = 0;   ///< GetTickCount64 到期时刻
};

/** 一次后台注入的结果，由工作线程写、主线程取。 */
struct InjectResult {
    DWORD       pid = 0;
    std::string exe;
    bool        ok = false;
    std::string err;
};

/** 常驻宿主的全部可变状态；只在主线程访问（WndProc、扫描、注入都在主线程）。 */
struct State {
    std::wstring configPath;                 ///< 配置文件绝对路径
    AppConfig    config;                     ///< 当前生效配置

    FILETIME    configStamp{};               ///< 磁盘最后写入时间，用于热重载判定
    std::string configRaw;                   ///< 磁盘原文，避免用内容相同的时间戳重载
    bool        configDirty = false;         ///< 界面有未保存修改

    std::vector<DWORD>         injectedPids; ///< 已注入的进程，避免重复注入
    std::vector<DWORD>         injecting;    ///< 已交给后台线程、尚未回结果的进程
    std::vector<PendingInject> pending;      ///< 等待延迟到期的注入
    ULONGLONG                  lastScanTick  = 0;
    ULONGLONG                  lastCheckTick = 0;

    std::vector<LogEntry> log;
    bool            showWindow  = true;      ///< 主窗口是否可见（false = 收在托盘）
    bool            requestExit = false;
    NOTIFYICONDATAW nid{};
    int             trayShownCount = -1;     ///< 托盘提示里已显示的注入数，避免无谓地调用 shell
};

/** 宽字符串转 UTF-8（界面与日志显示用）。 */
inline std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

/** 取路径所在目录；无分隔符时返回 "."。 */
inline std::wstring DirOf(const std::wstring& path)
{
    const size_t sep = path.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? std::wstring(L".") : path.substr(0, sep);
}

/** 取当前可执行文件所在目录。 */
inline std::wstring SelfDir()
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return DirOf(self);
}

/** 一个可供界面添加的 hook 组件（来自 injector.dll 的导出目录）。 */
struct ComponentInfo {
    std::string name;
    std::string defaultConfigYaml;   ///< 描述该组件默认参数的 YAML 片段，可为空
};

/**
 * 可添加的组件目录。
 *
 * 首次调用时从 injector.dll 的导出读取；DLL 缺失或导出不可用时返回空表，
 * 此时界面仍能编辑配置里已有的组件，只是无法“新增组件”。
 */
const std::vector<ComponentInfo>& AvailableComponents();

/** 打开 %TEMP%\WorkAreaFullscreen.log（覆盖写）；重复调用为空操作。 */
void OpenLogFile();

/** 追加一条界面日志，同时写入日志文件。 */
void Log(State& st, const char* fmt, ...);

/** 把主窗口收进托盘。 */
void HideMainWindow(State& st);

/** 配置文件不存在时写入默认配置。 @return 文件已存在（含刚创建）为 true。 */
bool EnsureConfigFile(const std::wstring& path);

// ---- 配置读写 ----

/** 从磁盘读取配置并替换 st.config。 @return 失败返回 false 并记录日志。 */
bool ReloadConfig(State& st);

/** 把 st.config 写回磁盘（注释头会被重写）。 @return 成功为 true。 */
bool SaveConfig(State& st);

// ---- 界面 ----

/** 绘制一帧界面。 */
void DrawUi(State& st);

// ---- 进程监听与注入 ----

/** 清理已退出进程，扫描目标列表，把新发现的目标排入 pending。 */
void WatchStep(State& st);

/** 处理到期的注入：提交给后台线程，本函数不做阻塞调用。 */
void ProcessPendingInjections(State& st);

/** 取回后台注入线程的结果并记日志（主线程每帧调用）。 */
void CollectInjectResults(State& st);

/** 单次模式：按配置启动目标并注入，等待目标退出。 @return 目标退出码。 */
int RunInjectOnce(const std::wstring& configPath, const std::wstring& exePath, int delayMsOverride);

// ---- 入口 ----

/** 常驻模式：窗口 + 托盘 + 主循环。 @return 进程退出码。 */
int RunUi(const std::wstring& configPath);

} // namespace waf::app
