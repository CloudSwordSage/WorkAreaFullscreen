// config_io.cpp - 配置文件定位与读取。
#include "waf/config_io.h"

#include <windows.h>

namespace waf {

std::wstring FindConfigPath(const std::wstring& nearDir)
{
    wchar_t buf[MAX_PATH]{};
    DWORD   n = GetEnvironmentVariableW(L"HOOKKIT_CONFIG", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;

    // 便携部署：与调用方同目录
    std::wstring dir = nearDir;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    const std::wstring portable = dir + L"WorkAreaFullscreen.yaml";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable;

    // 安装部署：用户配置目录
    wchar_t appdata[MAX_PATH]{};
    n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring installed = std::wstring(appdata) + L"\\WorkAreaFullscreen\\config.yaml";
        if (GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES) return installed;
    }

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"WorkAreaFullscreen.yaml";
}

bool ReadAllUtf8(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 1u << 20) { CloseHandle(h); return false; }

    out.resize(size);
    DWORD got = 0;
    const BOOL ok = (size == 0) || ReadFile(h, &out[0], size, &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

} // namespace waf
