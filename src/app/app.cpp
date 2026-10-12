// app.cpp - 主界面：Win32 窗口 + D3D11 + ImGui、配置读写与热重载、启动代理管理。
//
// 本程序不监听进程、也不注入；注入由 HookLauncher.exe 在目标执行第一条指令之前完成。
#include "app.h"

#include <d3d11.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "waf/ifeo.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace waf::app {
namespace {

constexpr wchar_t kWndClass[] = L"WorkAreaFullscreenWnd";

ID3D11Device*           g_device  = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swap    = nullptr;
ID3D11RenderTargetView* g_rtv     = nullptr;
State*                  g_state   = nullptr;
HWND                    g_hwnd    = nullptr;

const char* kConfigHeader =
    "# WorkAreaFullscreen 配置\n"
    "# 由界面保存时重写；手工编辑后程序会自动热重载。\n"
    "#\n"
    "# geometry          work_area = 窗口铺满工作区（保留任务栏）；monitor = 显示器整块区域\n"
    "# 启动代理的挂载状态存放在注册表（IFEO）里，不写进本文件。\n"
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
        if (g_state) g_state->requestExit = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 组件目录只存在于 injector.dll 的注册表里，这里通过它的导出接口取一次。
std::vector<ComponentInfo> g_catalog;

void LoadComponentCatalog()
{
    static bool loaded = false;
    if (loaded) return;
    loaded = true;

    const std::wstring path = SelfDir() + L"\\injector.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    // 让 DLL 知道这次加载只为读目录：绝不能在宿主进程里初始化 hook。
    SetEnvironmentVariableW(L"HOOKKIT_QUERY", L"1");
    HMODULE mod = LoadLibraryW(path.c_str());
    SetEnvironmentVariableW(L"HOOKKIT_QUERY", nullptr);
    if (!mod) return;

    typedef int(__stdcall * FnCount)(void);
    typedef const char*(__stdcall * FnName)(int);
    typedef const char*(__stdcall * FnConfig)(int);

    const auto count  = reinterpret_cast<FnCount>(GetProcAddress(mod, "HkComponentCount"));
    const auto nameOf = reinterpret_cast<FnName>(GetProcAddress(mod, "HkComponentName"));
    const auto cfgOf  = reinterpret_cast<FnConfig>(GetProcAddress(mod, "HkComponentDefaultConfig"));
    if (!count || !nameOf || !cfgOf) return;

    const int n = count();
    for (int i = 0; i < n; ++i) {
        const char* nm = nameOf(i);
        const char* cf = cfgOf(i);
        if (nm) g_catalog.push_back(ComponentInfo{ nm, cf ? cf : "" });
    }
}

/** 非管理员时用 runas 拉起自己执行 --ifeo-* 并等它结束。 */
bool RunElevatedIfeo(const std::string& exeName, bool on)
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    wchar_t args[MAX_PATH + 64];
    _snwprintf_s(args, _TRUNCATE, L"%s \"%S\"",
                 on ? L"--ifeo-install" : L"--ifeo-uninstall", exeName.c_str());

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"runas";
    sei.lpFile       = self;
    sei.lpParameters = args;
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        const std::wstring msg = (e == ERROR_CANCELLED)
            ? L"已取消：没有获得管理员授权，启动代理未改动。"
            : (L"请求管理员权限失败（err=" + std::to_wstring(e) + L"）。");
        MessageBoxW(g_hwnd, msg.c_str(), L"WorkAreaFullscreen", MB_ICONWARNING);
        return false;
    }

    DWORD code = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    if (code != 0) {
        MessageBoxW(g_hwnd, L"提权后的操作没有成功完成，请查看注册表权限或稍后重试。",
                    L"WorkAreaFullscreen", MB_ICONERROR);
        return false;
    }
    return true;
}

} // namespace

std::wstring LauncherPath() { return SelfDir() + L"\\HookLauncher.exe"; }

const std::vector<ComponentInfo>& AvailableComponents()
{
    LoadComponentCatalog();
    return g_catalog;
}

bool EnsureConfigFile(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return WriteAllBytes(path, DefaultConfig().Dump(kConfigHeader));
}

bool ReloadConfig(State& st)
{
    std::string text;
    if (!ReadAllBytes(st.configPath, text)) return false;

    AppConfig cfg;
    std::string err;
    if (!AppConfig::Parse(text, cfg, err)) return false;

    st.config      = std::move(cfg);
    st.configRaw   = std::move(text);
    st.configDirty = false;
    {
        std::string ignored;
        FileStamp(st.configPath, st.configStamp, ignored);   // 刷新时间戳
    }
    return true;
}

bool SaveConfig(State& st)
{
    const std::string text = st.config.Dump(kConfigHeader);
    if (!WriteAllBytes(st.configPath, text)) return false;

    st.configRaw   = text;
    st.configDirty = false;
    {
        std::string ignored;
        FileStamp(st.configPath, st.configStamp, ignored);   // 刷新时间戳
    }
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
        st.configStamp = stamp;   // 界面有未保存改动，忽略外部版本
        return;
    }
    ReloadConfig(st);
}

bool IsLaunchProxyOn(const std::string& exeName)
{
    return waf::IfeoIsInstalled(exeName);
}

bool ApplyLaunchProxy(const std::string& exeName, bool on)
{
    if (exeName.empty()) {
        MessageBoxW(g_hwnd, L"请先填写进程名。", L"WorkAreaFullscreen", MB_ICONWARNING);
        return false;
    }

    // 已是管理员就直接写；否则提权后再写。
    if (!waf::IsProcessElevated()) return RunElevatedIfeo(exeName, on);

    std::string err;
    const bool ok = on ? waf::IfeoInstall(exeName, L"\"" + LauncherPath() + L"\"", err)
                       : waf::IfeoUninstall(exeName, err);
    if (!ok) {
        const std::wstring msg = L"操作失败：" + std::wstring(err.begin(), err.end());
        MessageBoxW(g_hwnd, msg.c_str(), L"WorkAreaFullscreen", MB_ICONERROR);
    }
    return ok;
}

int RunIfeoCommand(const std::string& exeName, bool on)
{
    // 提权子进程不该出现控制台窗口；主程序是 console 子系统，这里直接摘掉。
    FreeConsole();

    std::string err;
    const bool ok = on ? waf::IfeoInstall(exeName, L"\"" + LauncherPath() + L"\"", err)
                       : waf::IfeoUninstall(exeName, err);
    return ok ? 0 : 1;
}

int RunUi(const std::wstring& configPath)
{
    State st;
    st.configPath = configPath;
    ReloadConfig(st);   // 失败也继续，界面里可以改

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
                            CW_USEDEFAULT, CW_USEDEFAULT, 980, 720,
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
    io.IniFilename = nullptr;   // 布局固定，不在工作目录留 imgui.ini
    LoadFont(io);
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi(st);
        ImGui::Render();

        const float clear[4] = { 0.09f, 0.10f, 0.12f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT present = g_swap->Present(1, 0);
        if (present == DXGI_STATUS_OCCLUDED) Sleep(50);   // 被完全遮挡时不要空转
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(kWndClass, wc.hInstance);
    g_state = nullptr;
    return 0;
}

} // namespace waf::app
