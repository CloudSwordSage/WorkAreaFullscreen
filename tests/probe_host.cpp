// probe_host.cpp - 集成自检用的目标进程。
//
// 打印被 hook API 的返回值（显示器矩形与 DC 尺寸），供 run_integration.ps1 断言。
// 不是产品的一部分，仅在 BUILD_TESTING 下构建。
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

namespace {

// 诊断输出：本机每个显示器的几何，以及 CreateDC 出来的 DC 经 GetDCOrgEx + MonitorFromPoint
// 会被映射到哪一个显示器。用于核对 runtime 的 MonitorOfDC 是否选对了显示器。
void DumpDisplays()
{
    int idx = 0;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        int& i = *reinterpret_cast<int*>(lp);

        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;

        printf("[probe] display[%d] %ls rcMonitor=(%ld,%ld)-(%ld,%ld) rcWork=(%ld,%ld)-(%ld,%ld)\n",
               i, mi.szDevice,
               mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
               mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
        ++i;

        HDC dc = CreateDCW(mi.szDevice, nullptr, nullptr, nullptr);
        if (dc) {
            POINT        org{ -1, -1 };
            const BOOL   ok      = GetDCOrgEx(dc, &org);
            const HMONITOR byOrg = ok ? MonitorFromPoint(org, MONITOR_DEFAULTTONULL) : nullptr;
            printf("[probe]   %ls: GetDCOrgEx=%d org=(%ld,%ld) -> %s  caps=%dx%d\n",
                   mi.szDevice, (int)ok, org.x, org.y,
                   (byOrg == mon) ? "SAME" : "DIFFERENT",
                   GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES));
            DeleteDC(dc);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&idx));
}

// 模拟 SDL_CreateWindow 解析 SDL_WINDOWPOS_CENTERED 的结果：
//   bounds 来自 SDL_GetDisplayBounds（内部读 GetMonitorInfoW 的 rcMonitor）
//   尺寸来自 SDL display desktop_mode（内部读 GetDeviceCaps 的 HORZRES/VERTRES）
// 两者都被 hook 改写后，结果应当等于工作区矩形。
void DumpSimulatedWindow(const char* tag)
{
    const HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    HDC dc = CreateDCW(mi.szDevice, nullptr, nullptr, nullptr);
    if (!dc) return;
    const int w = GetDeviceCaps(dc, HORZRES);
    const int h = GetDeviceCaps(dc, VERTRES);
    DeleteDC(dc);

    const int bw = mi.rcMonitor.right - mi.rcMonitor.left;
    const int bh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int x  = mi.rcMonitor.left + (bw - w) / 2;
    const int y  = mi.rcMonitor.top + (bh - h) / 2;

    printf("[probe] %s WINDOW pos=(%d,%d) size=(%dx%d)\n", tag, x, y, w, h);
}

void Dump(const char* tag)
{
    const HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi = { sizeof(mi) };
    if (mon && GetMonitorInfoW(mon, &mi)) {
        printf("[probe] %s MONITOR rcMonitor=(%ld,%ld)-(%ld,%ld) rcWork=(%ld,%ld)-(%ld,%ld)\n", tag,
               mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
               mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
    }

    if (HDC dc = GetDC(NULL)) {
        printf("[probe] %s CAPS HORZRES=%d VERTRES=%d\n", tag,
               GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES));
        ReleaseDC(NULL, dc);
    }

    DumpSimulatedWindow(tag);
    fflush(stdout);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // 第二个参数为等待注入的毫秒数：先打印基线，等注入生效，再打印结果。
    const int waitMs = (argc > 1) ? _wtoi(argv[1]) : 3000;

    DumpDisplays();
    Dump("before");
    Sleep(static_cast<DWORD>(waitMs));
    Dump("after");
    return 0;
}
