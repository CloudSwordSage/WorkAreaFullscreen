// app.cpp - 常驻宿主：Win32 窗口 + D3D11 + ImGui、托盘、配置读写与热重载。
#include "app.h"

#include <d3d11.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace waf::app {
namespace {

constexpr UINT   kTrayMsg = WM_APP + 1;
constexpr UINT   kTrayId  = 1;
constexpr wchar_t kWndClass[] = L"WorkAreaFullscreenWnd";

ID3D11Device*           g_device  = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swap    = nullptr;
ID3D11RenderTargetView* g_rtv     = nullptr;
HWND                    g_hwnd    = nullptr;
State*                  g_state   = nullptr;
HANDLE                  g_logFile = INVALID_HANDLE_VALUE;

const char* kConfigHeader =
    "# WorkAreaFullscreen 配置\n"
    "# 由界面保存时重写；手工编辑后程序会自动热重载。\n"
    "#\n"
    "# inject_delay_ms   发现目标进程后延迟注入的毫秒数（立即注入会让部分引擎崩溃）\n"
    "# scan_interval_ms  目标进程扫描间隔\n"
    "# geometry          work_area = 窗口铺满工作区（保留任务栏）；monitor = 显示器整块区域\n"
    "\n";

bool ReadAllBytes(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 4u << 20) { CloseHandle(h); return false; }

    out.resize(size);
    DWORD got = 0;
    const BOOL ok = (size == 0) || ReadFile(h, &out[0], size, &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

bool WriteAllBytes(const std::wstring& path, const std::string& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = text.empty() || WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(h);
    return ok && w == text.size();
}

bool FileStamp(const std::wstring& path, FILETIME& stamp, std::string& raw)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    stamp = fad.ftLastWriteTime;
    if (!ReadAllBytes(path, raw)) raw.clear();
    return true;
}

// ---- D3D11 ----

void CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL       got      = D3D_FEATURE_LEVEL_10_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // 无独显/远程桌面时退回软件渲染
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swap)    { g_swap->Release();    g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

void ResizeSwapChain()
{
    if (!g_swap) return;
    CleanupRenderTarget();
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    g_swap->ResizeBuffers(0, (UINT)(rc.right - rc.left), (UINT)(rc.bottom - rc.top),
                          DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

void LoadFont(ImGuiIO& io)
{
    const wchar_t* candidates[] = {
        L"C:\\Windows\\Fonts\\msyh.ttc",
        L"C:\\Windows\\Fonts\\msyh.ttf",
        L"C:\\Windows\\Fonts\\simhei.ttf",
    };
    for (const wchar_t* p : candidates) {
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) continue;
        const std::string mb = Narrow(p);
        if (io.Fonts->AddFontFromFileTTF(mb.c_str(), 18.0f, nullptr,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
            return;
    }
    io.Fonts->AddFontDefault();
}

// ---- 托盘 ----

void ShowMainWindow(State& st)
{
    st.showWindow = true;
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

void CreateTray(State& st)
{
    st.nid = NOTIFYICONDATAW{};
    st.nid.cbSize           = sizeof(st.nid);
    st.nid.hWnd             = g_hwnd;
    st.nid.uID              = kTrayId;
    st.nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    st.nid.uCallbackMessage = kTrayMsg;
    st.nid.hIcon            = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcsncpy_s(st.nid.szTip, L"WorkAreaFullscreen", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &st.nid);
}

void UpdateTrayTip(State& st)
{
    const int active = (int)st.injectedPids.size();
    _snwprintf_s(st.nid.szTip, _TRUNCATE, L"WorkAreaFullscreen — 已注入 %d 个进程", active);
    st.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &st.nid);
}

void RemoveTray(State& st)
{
    if (st.nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &st.nid);
}

void ShowTrayMenu(State& st)
{
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"显示主窗口");
    AppendMenuW(menu, MF_STRING, 2, L"重新加载配置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"退出");

    const int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                   pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 1) {
        ShowMainWindow(st);
    } else if (cmd == 2) {
        ReloadConfig(st);
    } else if (cmd == 3) {
        st.requestExit = true;
    }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
    case WM_SIZE:
        if (g_device && wp != SIZE_MINIMIZED) ResizeSwapChain();
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;   // 屏蔽 Alt 菜单
        break;
    case WM_CLOSE:
        if (g_state && g_state->config.minimizeToTray) {
            ShowWindow(hwnd, SW_HIDE);
            g_state->showWindow = false;
        } else if (g_state) {
            g_state->requestExit = true;
        }
        return 0;
    case kTrayMsg:
        if (!g_state) return 0;
        if (lp == WM_LBUTTONDBLCLK)      ShowMainWindow(*g_state);
        else if (lp == WM_RBUTTONUP)     ShowTrayMenu(*g_state);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void HideMainWindow(State& st)
{
    st.showWindow = false;
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
    Log(st, "已收进托盘");
}

bool EnsureConfigFile(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const std::string text = DefaultConfig().Dump(kConfigHeader);
    if (!WriteAllBytes(path, text)) return false;
    return true;
}

void OpenLogFile()
{    if (g_logFile != INVALID_HANDLE_VALUE) return;
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring logPath = std::wstring(tmp) + L"WorkAreaFullscreen.log";
    g_logFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(State& st, const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    char ts[16];
    _snprintf_s(ts, sizeof(ts), _TRUNCATE, "%02u:%02u:%02u", lt.wHour, lt.wMinute, lt.wSecond);

    st.log.push_back({ ts, body });
    if (st.log.size() > 800) st.log.erase(st.log.begin(), st.log.begin() + 200);

    if (g_logFile != INVALID_HANDLE_VALUE) {
        char line[1200];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[%s] %s\r\n", ts, body);
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)strlen(line), &w, nullptr);
    }
}

bool ReloadConfig(State& st)
{
    std::string text;
    if (!ReadAllBytes(st.configPath, text)) {
        Log(st, "读取配置失败：%s", Narrow(st.configPath).c_str());
        return false;
    }

    AppConfig cfg;
    std::string err;
    if (!AppConfig::Parse(text, cfg, err)) {
        Log(st, "配置解析失败：%s", err.c_str());
        return false;
    }

    st.config      = std::move(cfg);
    st.configRaw   = std::move(text);
    st.configDirty = false;
    {
        std::string ignored;
        FileStamp(st.configPath, st.configStamp, ignored);   // 刷新时间戳
    }

    Log(st, "配置已加载：%zu 条规则，延迟 %d ms，扫描间隔 %d ms",
        st.config.targets.size(), st.config.injectDelayMs, st.config.scanIntervalMs);
    return true;
}

bool SaveConfig(State& st)
{
    const std::string text = st.config.Dump(kConfigHeader);
    if (!WriteAllBytes(st.configPath, text)) {
        Log(st, "写入配置失败：%s", Narrow(st.configPath).c_str());
        return false;
    }
    st.configRaw   = text;
    st.configDirty = false;
    {
        std::string ignored;
        FileStamp(st.configPath, st.configStamp, ignored);   // 刷新时间戳
    }
    Log(st, "配置已保存");
    return true;
}

// 外部改动（编辑器保存）时热重载；界面有未保存修改时不覆盖。
static void CheckConfigReload(State& st)
{
    FILETIME stamp{};
    std::string raw;
    if (!FileStamp(st.configPath, stamp, raw)) return;
    if (CompareFileTime(&stamp, &st.configStamp) == 0) return;
    if (raw == st.configRaw) { st.configStamp = stamp; return; }   // 内容未变

    if (st.configDirty) {
        Log(st, "检测到外部修改，但界面有未保存改动，已忽略外部版本");
        st.configStamp = stamp;
        return;
    }
    Log(st, "检测到配置文件变化，正在热重载");
    ReloadConfig(st);
}

int RunUi(const std::wstring& configPath)
{
    State st;
    st.configPath = configPath;
    ReloadConfig(st);   // 失败也继续，界面里可以改
    OpenLogFile();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hIcon         = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    g_state = &st;
    g_hwnd  = CreateWindowW(kWndClass, L"WorkAreaFullscreen", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
                            nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"创建窗口失败", L"WorkAreaFullscreen", MB_ICONERROR);
        UnregisterClassW(kWndClass, wc.hInstance);
        return 1;
    }

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(g_hwnd);
        UnregisterClassW(kWndClass, wc.hInstance);
        MessageBoxW(nullptr, L"初始化 D3D11 失败", L"WorkAreaFullscreen", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    LoadFont(io);
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    CreateTray(st);
    UpdateTrayTip(st);
    Log(st, "WorkAreaFullscreen 已启动");

    while (!st.requestExit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (st.requestExit) break;

        const ULONGLONG now = GetTickCount64();
        if (now - st.lastCheckTick >= 1000) {
            st.lastCheckTick = now;
            CheckConfigReload(st);
        }
        if (st.config.enabled && now - st.lastScanTick >= (ULONGLONG)st.config.scanIntervalMs) {
            st.lastScanTick = now;
            WatchStep(st);
            UpdateTrayTip(st);
        }

        if (!st.showWindow) {   // 收进托盘时不渲染，只做后台工作
            Sleep(50);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi(st);
        ImGui::Render();

        const float clear[4] = { 0.09f, 0.10f, 0.12f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);   // 垂直同步
    }

    RemoveTray(st);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(kWndClass, wc.hInstance);
    g_state = nullptr;

    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    return 0;
}

} // namespace waf::app
