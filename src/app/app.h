// app.h - WorkAreaFullscreen：配置编辑器 + 启动代理（IFEO）管理。
//
// 本程序不监听进程、不注入；注入由 HookLauncher.exe 在目标执行第一条指令之前完成。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "waf/config.h"

namespace waf::app {

/** 一个可供界面添加的 hook 组件（来自 injector.dll 的导出目录）。 */
struct ComponentInfo {
    std::string name;
    std::string defaultConfigYaml;   ///< 描述该组件默认参数的 YAML 片段，可为空
};

/** 程序状态；只在主线程访问。 */
struct State {
    std::wstring configPath;                 ///< 配置文件绝对路径
    AppConfig    config;                     ///< 当前生效配置

    FILETIME    configStamp{};               ///< 磁盘最后写入时间，用于热重载判定
    std::string configRaw;                   ///< 磁盘原文，避免用内容相同的时间戳重载
    bool        configDirty   = false;       ///< 界面有未保存修改
    bool        requestExit   = false;
    ULONGLONG   lastCheckTick = 0;           ///< 配置热重载的检查节流
};

/** 宽字符串转 UTF-8（界面显示用）。 */
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

/**
 * 可添加的组件目录。
 *
 * 首次调用时从 injector.dll 的导出读取；DLL 缺失或导出不可用时返回空表，
 * 此时界面仍能编辑配置里已有的组件，只是无法“新增组件”。
 */
const std::vector<ComponentInfo>& AvailableComponents();

/** 配置文件不存在时写入默认配置。 @return 文件已存在（含刚创建）为 true。 */
bool EnsureConfigFile(const std::wstring& path);

/** 从磁盘读取配置并替换 st.config。 @return 失败返回 false。 */
bool ReloadConfig(State& st);

/** 把 st.config 写回磁盘。 @return 成功为 true。 */
bool SaveConfig(State& st);

/** 绘制一帧界面。 */
void DrawUi(State& st);

/** 本程序同目录下的启动代理路径。 */
std::wstring LauncherPath();

// ---- 启动代理（IFEO）----

/** 该目标的启动代理当前是否已挂载。 */
bool IsLaunchProxyOn(const std::string& exeName);

/**
 * 挂载或摘除某目标的启动代理。
 *
 * 当前进程已是管理员时直接写注册表；否则用 ShellExecuteEx + runas 提权，由本程序带
 * `--ifeo-install` / `--ifeo-uninstall` 再执行一次，并等待其结束。提权被取消或执行
 * 失败时会弹出对话框说明。
 *
 * @param exeName 映像名，如 stellaris.exe。
 * @param on      true 挂载，false 摘除。
 * @return 最终成功为 true。
 */
bool ApplyLaunchProxy(const std::string& exeName, bool on);

/** 提权模式入口：由 ApplyLaunchProxy 拉起，执行完即退出。 @return 进程退出码。 */
int RunIfeoCommand(const std::string& exeName, bool on);

// ---- 入口 ----

/** 主界面：配置编辑 + 启动代理管理。 @return 进程退出码。 */
int RunUi(const std::wstring& configPath);

} // namespace waf::app
