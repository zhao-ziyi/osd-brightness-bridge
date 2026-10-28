/* bridge.c — osd-brightness-bridge
 *
 * 把 Windows 系统亮度弹窗（OSD）的值通过 DDC/CI 镜像给外接显示器，
 * 让 Fn 亮度键在台式机外接屏上真正生效。
 *
 * 原理（实测验证）：
 *   按 Fn 亮度键时，Windows 自己弹 OSD 并把亮度值写入电源方案
 *   SUB_VIDEO\VIDEONORMALLEVEL（0~100%），但台式机外接显示器场景下
 *   系统不把该值通过 DDC/CI 下发。本程序只做"无状态镜像"：
 *
 *   [Raw Input 捕获 Consumer 0x6F/0x70 帧] → 等 120ms（让系统写完值）
 *       → PowerReadACValueIndex(VIDEONORMALLEVEL) → SetMonitorBrightness(该值)
 *
 *   程序自身不记任何状态：OSD 显示什么，显示器就是什么。
 *
 * 托盘：右键菜单「开机自启」（HKCU\...\Run）与「退出」。
 *
 * 编译（VS x64 Native Tools）：
 *   cl /O2 /W3 /utf-8 bridge.c /link user32.lib powrprof.lib dxva2.lib shell32.lib advapi32.lib /SUBSYSTEM:WINDOWS
 *
 * 占用：单 exe ~100KB，空闲私有内存 ~2MB，CPU 0（事件驱动，无轮询）。
 * License: MIT
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

/* 电源方案：显示子组 7516b95f-f776-4464-8c53-06167f40cc99
 *          显示器亮度 aded5e82-b909-4619-9949-f5d71dac0bcb (VIDEONORMALLEVEL) */
static const GUID GUID_VIDEO_SUBGROUP   = {0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99}};
static const GUID GUID_VIDEONORMALLEVEL = {0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb}};

#define TIMER_APPLY      1
#define APPLY_DELAY      120            /* ms：去抖 + 等系统写值 */

#define WM_TRAYICON      (WM_APP + 1)
#define ID_TRAY          1
#define IDM_AUTOSTART    1001
#define IDM_EXIT         1002

#define RUN_KEY          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE        L"OSDBrightnessBridge"

/* ---- 读系统亮度值（OSD 弹窗条所对应的"活值"） ---- */
static BOOL read_system_brightness(DWORD *out)
{
    GUID *scheme = NULL;
    if (PowerGetActiveScheme(NULL, &scheme) != ERROR_SUCCESS)
        return FALSE;

    SYSTEM_POWER_STATUS sps = {0};
    GetSystemPowerStatus(&sps);

    DWORD val = 0, rc;
    if (sps.ACLineStatus == 1)
        rc = PowerReadACValueIndex(NULL, scheme, &GUID_VIDEO_SUBGROUP, &GUID_VIDEONORMALLEVEL, &val);
    else
        rc = PowerReadDCValueIndex(NULL, scheme, &GUID_VIDEO_SUBGROUP, &GUID_VIDEONORMALLEVEL, &val);

    LocalFree(scheme);
    if (rc != ERROR_SUCCESS)
        return FALSE;
    *out = val;
    return TRUE;
}

/* ---- 把值镜像到所有支持 DDC 的物理显示器 ---- */
struct mirror_ctx { DWORD value; };

static BOOL CALLBACK enum_mon_proc(HMONITOR hMon, HDC hdc, LPRECT lprc, LPARAM lParam)
{
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &n) || n == 0)
        return TRUE;

    LPPHYSICAL_MONITOR pms = (LPPHYSICAL_MONITOR)HeapAlloc(GetProcessHeap(), 0, n * sizeof(PHYSICAL_MONITOR));
    if (!pms)
        return TRUE;
    if (GetPhysicalMonitorsFromHMONITOR(hMon, n, pms)) {
        DWORD pct = ((struct mirror_ctx *)lParam)->value;   /* 0~100 */
        for (DWORD i = 0; i < n; i++) {
            DWORD mn = 0, cur = 0, mx = 0;
            if (GetMonitorBrightness(pms[i].hPhysicalMonitor, &mn, &cur, &mx)) {
                DWORD target = mn + (mx - mn) * pct / 100;
                if (target != cur)
                    SetMonitorBrightness(pms[i].hPhysicalMonitor, target);
            }
        }
        DestroyPhysicalMonitors(n, pms);
    }
    HeapFree(GetProcessHeap(), 0, pms);
    return TRUE;
}

static void apply_brightness(void)
{
    DWORD val;
    if (!read_system_brightness(&val))
        return;
    struct mirror_ctx ctx = { val };
    EnumDisplayMonitors(NULL, NULL, enum_mon_proc, (LPARAM)&ctx);
}

/* ---- 开机自启：HKCU\...\Run ---- */
static BOOL autostart_is_enabled(void)
{
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &hkey) != ERROR_SUCCESS)
        return FALSE;
    LONG rc = RegQueryValueExW(hkey, RUN_VALUE, NULL, NULL, NULL, NULL);
    RegCloseKey(hkey);
    return rc == ERROR_SUCCESS;
}

static void autostart_set(BOOL enable)
{
    HKEY hkey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &hkey, NULL) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        wchar_t quoted[MAX_PATH + 3];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        lstrcpyW(quoted, L"\"");
        lstrcatW(quoted, path);
        lstrcatW(quoted, L"\"");
        RegSetValueExW(hkey, RUN_VALUE, 0, REG_SZ,
                       (const BYTE *)quoted, (DWORD)((lstrlenW(quoted) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hkey, RUN_VALUE);
    }
    RegCloseKey(hkey);
}

/* ---- 托盘图标 ---- */
static void tray_add(HWND hwnd)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize   = sizeof(nid);
    nid.hWnd     = hwnd;
    nid.uID      = ID_TRAY;
    nid.uFlags   = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon    = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpyW(nid.szTip, L"osd-brightness-bridge");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void tray_del(HWND hwnd)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = ID_TRAY;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void tray_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING | (autostart_is_enabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    /* 经典坑：不先 SetForegroundWindow 菜单点完不消失 */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ---- 隐藏消息窗口：收 Raw Input + 托盘回调 ---- */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INPUT: {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        if (size > 0 && size < 4096) {
            static BYTE buf[4096];
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
                RAWINPUT *raw = (RAWINPUT *)buf;
                if (raw->header.dwType == RIM_TYPEHID && raw->data.hid.dwSizeHid >= 3) {
                    const BYTE *d = raw->data.hid.bRawData;
                    /* 实测报文：Report ID 3, [usage]：0x6F=亮度+ 0x70=亮度-，抬起 03 00 00 */
                    if (d[0] == 0x03 && (d[1] == 0x6F || d[1] == 0x70)) {
                        KillTimer(hwnd, TIMER_APPLY);          /* 去抖：以最后一次为准 */
                        SetTimer(hwnd, TIMER_APPLY, APPLY_DELAY, NULL);
                    }
                }
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == TIMER_APPLY) {
            KillTimer(hwnd, TIMER_APPLY);
            apply_brightness();
        }
        return 0;
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP || lParam == WM_CONTEXTMENU)
            tray_menu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_AUTOSTART:
            autostart_set(!autostart_is_enabled());
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        tray_del(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int show)
{
    (void)hPrev; (void)cmdLine; (void)show;

    /* 单实例 */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"OSDBrightnessBridgeSingleton");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"OSDBrightnessBridgeWnd";
    if (!RegisterClassW(&wc))
        return 1;

    /* 隐藏的顶层工具窗（托盘回调需要普通窗口；WS_EX_TOOLWINDOW 不出任务栏） */
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"OSD Brightness Bridge",
                                WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 1;

    /* 监听 Consumer 页(0x0C) 键盘类(0x01)，INPUTSINK = 后台也收 */
    RAWINPUTDEVICE rid = { 0x0C, 0x01, RIDEV_INPUTSINK, hwnd };
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        return 1;

    tray_add(hwnd);

    /* 启动时先同步一次（重启后显示器与系统值可能不一致） */
    apply_brightness();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
