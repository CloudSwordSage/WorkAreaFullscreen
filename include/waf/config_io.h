// config_io.h - 配置文件定位与读取（注入模块与启动代理共用）。
#pragma once
#include <string>

namespace waf {

/**
 * 定位配置文件。
 *
 * 查找顺序：HOOKKIT_CONFIG 环境变量 > nearDir\WorkAreaFullscreen.yaml（便携部署）
 *          > %APPDATA%\WorkAreaFullscreen\config.yaml（安装部署）> %TEMP%\WorkAreaFullscreen.yaml。
 *
 * @param nearDir 调用方所在目录（DLL 或启动代理所在的目录）。
 * @return 选中的路径；最后一项只是兜底，不保证存在。
 */
std::wstring FindConfigPath(const std::wstring& nearDir);

/** 读整个文件为 UTF-8 字节。 @return 成功为 true（含空文件）。 */
bool ReadAllUtf8(const std::wstring& path, std::string& out);

} // namespace waf
