// probe_host.cpp - 集成自检用的目标进程。
//
// 打印被 hook API 的返回值（显示器矩形与 DC 尺寸），供 run_integration.ps1 断言。
// 不是产品的一部分，仅在 BUILD_TESTING 下构建。
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

namespace {

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

    fflush(stdout);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // 第二个参数为等待注入的毫秒数：先打印基线，等注入生效，再打印结果。
    const int waitMs = (argc > 1) ? _wtoi(argv[1]) : 3000;

    Dump("before");
    Sleep(static_cast<DWORD>(waitMs));
    Dump("after");
    return 0;
}
