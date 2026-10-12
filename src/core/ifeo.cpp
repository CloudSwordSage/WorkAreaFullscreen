// ifeo.cpp - Image File Execution Options 的读写实现。
#include "waf/ifeo.h"

#include <windows.h>

namespace waf {
namespace {

const wchar_t* kIfeoRoot =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring SubKeyOf(const std::string& exeName)
{
    return std::wstring(kIfeoRoot) + L"\\" + Widen(exeName);
}

} // namespace

bool IsProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD           size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool IfeoIsInstalled(const std::string& exeName, std::wstring* debuggerPath)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SubKeyOf(exeName).c_str(), 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS) {
        return false;
    }

    wchar_t       buf[MAX_PATH * 2] = {};
    DWORD         bytes             = sizeof(buf);
    DWORD         type              = 0;
    const LSTATUS st = RegQueryValueExW(key, L"Debugger", nullptr, &type,
                                        reinterpret_cast<BYTE*>(buf), &bytes);
    RegCloseKey(key);

    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    if (debuggerPath) *debuggerPath = buf;
    return true;
}

bool IfeoInstall(const std::string& exeName, const std::wstring& debuggerPath, std::string& err)
{
    HKEY          key = nullptr;
    const LSTATUS st  = RegCreateKeyExW(HKEY_LOCAL_MACHINE, SubKeyOf(exeName).c_str(), 0, nullptr,
                                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        err = (st == ERROR_ACCESS_DENIED) ? "需要管理员权限"
                                          : ("创建注册表键失败 err=" + std::to_string(st));
        return false;
    }

    const DWORD   bytes = static_cast<DWORD>((debuggerPath.size() + 1) * sizeof(wchar_t));
    const LSTATUS st2   = RegSetValueExW(key, L"Debugger", 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(debuggerPath.c_str()), bytes);
    RegCloseKey(key);
    if (st2 != ERROR_SUCCESS) {
        err = "写入 Debugger 值失败 err=" + std::to_string(st2);
        return false;
    }
    return true;
}

bool IfeoUninstall(const std::string& exeName, std::string& err)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SubKeyOf(exeName).c_str(), 0, KEY_SET_VALUE, &key)
        != ERROR_SUCCESS) {
        err = "未找到对应的 IFEO 项（本来就没挂过？）";
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

std::string ExeNameOf(const std::string& path)
{
    const size_t sep = path.find_last_of("\\/");
    return (sep == std::string::npos) ? path : path.substr(sep + 1);
}

} // namespace waf
