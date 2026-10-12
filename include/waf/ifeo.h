// ifeo.h - Image File Execution Options 的读写。
//
// 用来给某个可执行文件挂上“启动代理”：之后任何程序启动它，Windows 都会先启动代理程序，
// 并把目标路径作为第一个参数传过去。写 HKLM 需要管理员权限。
#pragma once
#include <string>

namespace waf {

/** 当前进程是否以管理员权限运行（写 IFEO 的前提）。 */
bool IsProcessElevated();

/**
 * 查询某映像名（如 "stellaris.exe"）是否已挂启动代理。
 *
 * @param exeName      映像名，不是完整路径。
 * @param debuggerPath 非空时写入已配置的代理程序路径。
 * @return 已配置返回 true。
 */
bool IfeoIsInstalled(const std::string& exeName, std::wstring* debuggerPath = nullptr);

/**
 * 给某映像名挂上启动代理。
 *
 * @param exeName      映像名，如 "stellaris.exe"。
 * @param debuggerPath 代理程序的完整路径（调用方负责加引号与否）。
 * @param err          失败原因。
 * @return 成功返回 true。
 */
bool IfeoInstall(const std::string& exeName, const std::wstring& debuggerPath, std::string& err);

/**
 * 摘掉启动代理。
 *
 * @param err 失败原因（键或值本就不存在也算失败，便于界面区分）。
 * @return 成功返回 true。
 */
bool IfeoUninstall(const std::string& exeName, std::string& err);

/** 从完整路径取出映像名。 */
std::string ExeNameOf(const std::string& path);

} // namespace waf
