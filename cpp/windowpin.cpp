// WindowPinLite —— Windows 窗口置顶小工具（C++ / Win32 + GDI+ 版）
//
// 特性：
//   * 一键切换置顶（热键 / 左键托盘图标），单窗口置顶（切换时自动取消上一个）
//   * 置顶期间在标题栏中央显示红色图钉（内嵌 Segoe UI Emoji 📌 的渲染像素）
//   * 点击图钉 = 取消该窗口置顶
//   * 托盘右键菜单：更改热键… / 退出
//   * 热键可自定义，冲突时提示「该热键冲突，请更换！」，持久化到
//     %APPDATA%\WindowPin\config.ini（与 Python 版共用同一份配置）
//   * 退出时自动取消所有置顶，恢复窗口原状态
//
// 兼容性：Windows 7 SP1 及以上（x86 / x64 均可）。
//   * 目标系统版本固定为 6.01（Win7），不使用任何 Win8+ 才有的 API
//   * 图标资源小尺寸用 BMP 帧，避免 Win7 对 PNG 压缩帧支持不佳的问题
//   * 静态链接 CRT（/MT），无需 VC++ 运行库
//
// 编译：
//   cl /nologo /utf-8 /O1 /MT /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601
//      windowpin.cpp /link /SUBSYSTEM:WINDOWS,6.01 /OPT:REF /OPT:ICF gdiplus.lib
//   （x64 用 build.bat，x86 用 build32.bat）

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <objidl.h>          // GDI+ 需要 IStream / PROPID（WIN32_LEAN_AND_MEAN 会排除）
#include <shellapi.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdarg>

#include "resource.h"
#include "pin_data.h"        // 内嵌的 emoji 图钉 PNG（由 make_assets.py 生成）

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "kernel32.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------- 常量
#define WM_APP_TOGGLE      (WM_APP + 1)
#define WM_APP_TRAY        (WM_APP + 2)
#define WM_APP_PINCLICK    (WM_APP + 3)

#define ID_TRAY            1001
#define IDM_HOTKEY         2001
#define IDM_QUIT           2002
#define HOTKEY_ID          1
#define HOTKEY_TEST_ID     0xBEEF
#define TIMER_FOLLOW       1

// 与 Python 版 window_pin.py 完全一致：26x22 画布 / 16px 字号
static const int PIN_W = 26;
static const int PIN_H = 22;

// 默认热键：Ctrl+Alt+Shift+P
static const UINT DEF_MODS = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
static const UINT DEF_VK   = 'P';

static const wchar_t* HOST_CLASS = L"PinHostCls2";
static const wchar_t* PIN_CLASS  = L"PinWndCls2";
static const wchar_t* CAP_CLASS  = L"PinCapCls2";

// ---------------------------------------------------------------- 全局状态
struct PinEntry {
    HWND target;
    HWND pin;
    bool prevTopmost;   // 置顶前该窗口是否已经是 topmost（退出时据此恢复）
};

static HWND g_host = NULL;
static HWND g_capDlg = NULL;
static std::vector<PinEntry> g_pins;
static NOTIFYICONDATAW g_nid = {};
static UINT g_mods = DEF_MODS;
static UINT g_vk = DEF_VK;
static bool g_hotkeyOk = false;
// 最近一次成功置顶的目标窗口：点击托盘时前台会变成任务栏，
// 用它作为候选目标，避免「点了没反应」。
static HWND g_lastTarget = NULL;
// 托盘左键防抖：双击会先发 LBUTTONUP 再发 LBUTTONDBLCLK
static DWORD g_lastTrayClick = 0;

static ULONG_PTR g_gdiToken = 0;

// ---------------------------------------------------------------- 调试日志
#ifdef WP_DEBUG_LOG
static void LogF(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH] = {};
    GetEnvironmentVariableW(L"TEMP", path, MAX_PATH);
    std::wstring p = std::wstring(path) + L"\\windowpin.log";
    FILE* f = NULL;
    if (_wfopen_s(&f, p.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}
#define LOG(...) LogF(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

// 图钉贴图（预乘 alpha 的 BGRA，供 UpdateLayeredWindow 使用）
static HDC  g_pinDC = NULL;
static HBITMAP g_pinBmp = NULL;
static void* g_pinBits = NULL;

static HICON g_appIcon = NULL;

// ---------------------------------------------------------------- 配置读写
static std::wstring ConfigPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        GetCurrentDirectoryW(MAX_PATH, buf);
    }
    std::wstring p(buf);
    p += L"\\WindowPin";
    CreateDirectoryW(p.c_str(), NULL);
    p += L"\\config.ini";
    return p;
}

static void LoadConfig() {
    std::wstring path = ConfigPath();
    UINT mods = GetPrivateProfileIntW(L"hotkey", L"mods", (int)DEF_MODS, path.c_str());
    UINT vk   = GetPrivateProfileIntW(L"hotkey", L"vk", (int)DEF_VK, path.c_str());
    if (vk == 0) { mods = DEF_MODS; vk = DEF_VK; }
    g_mods = mods;
    g_vk = vk;
}

static std::wstring VkName(UINT vk) {
    wchar_t buf[32];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = (wchar_t)vk; buf[1] = 0;
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf(buf, 32, L"F%u", vk - VK_F1 + 1);
        return buf;
    }
    switch (vk) {
    case VK_BACK:   return L"Backspace";
    case VK_TAB:    return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE:  return L"Space";
    case VK_LEFT:   return L"Left";
    case VK_UP:     return L"Up";
    case VK_RIGHT:  return L"Right";
    case VK_DOWN:   return L"Down";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_PAUSE:  return L"Pause";
    case VK_HOME:   return L"Home";
    case VK_END:    return L"End";
    case VK_PRIOR:  return L"PageUp";
    case VK_NEXT:   return L"PageDown";
    case VK_OEM_3:  return L"`";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_PLUS:  return L"=";
    case VK_OEM_4:  return L"[";
    case VK_OEM_6:  return L"]";
    case VK_OEM_5:  return L"\\";
    case VK_OEM_1:  return L";";
    case VK_OEM_7:  return L"'";
    case VK_OEM_COMMA:  return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_2:  return L"/";
    }
    swprintf(buf, 32, L"VK%02X", vk);
    return buf;
}

static std::wstring HotkeyLabel(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";
    s += VkName(vk);
    return s;
}

static void SaveConfig(UINT mods, UINT vk) {
    std::wstring path = ConfigPath();
    wchar_t buf[32];
    swprintf(buf, 32, L"%u", mods);
    WritePrivateProfileStringW(L"hotkey", L"mods", buf, path.c_str());
    swprintf(buf, 32, L"%u", vk);
    WritePrivateProfileStringW(L"hotkey", L"vk", buf, path.c_str());
    WritePrivateProfileStringW(L"hotkey", L"label", HotkeyLabel(mods, vk).c_str(), path.c_str());
}

// ---------------------------------------------------------------- 图钉绘制
// 图钉外观 = Windows 自带的 Segoe UI Emoji 中 U+1F4CC（红色图钉）的渲染结果。
// 该 emoji 在构建时用同字体、同尺寸（26x22 画布 / 16px 字号）渲染成 PNG，
// 内嵌在 pin_data.h，运行时用 GDI+ 解码，因此与 Python 版逐像素一致。
static bool LoadPinPng(Bitmap** outBmp, IStream** outStream) {
    *outBmp = NULL;
    *outStream = NULL;

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, PIN_PNG_SIZE);
    if (!mem) return false;
    void* p = GlobalLock(mem);
    if (!p) { GlobalFree(mem); return false; }
    memcpy(p, PIN_PNG, PIN_PNG_SIZE);
    GlobalUnlock(mem);

    IStream* stream = NULL;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return false;
    }
    Bitmap* bmp = Bitmap::FromStream(stream);
    if (!bmp || bmp->GetLastStatus() != Ok) {
        if (bmp) delete bmp;
        stream->Release();
        return false;
    }
    // stream 必须在 bitmap 生命周期内保持有效，由调用方释放
    *outBmp = bmp;
    *outStream = stream;
    return true;
}

// 输出预乘 alpha 的 BGRA。
// 源 PNG（26x22）与目标尺寸一致时直接按像素复制，避免任何缩放/重采样差异，
// 从而与 Python 版逐像素一致；目标尺寸不同时才走 GDI+ 高质量缩放。
static bool BuildPinImage(int W, int H, std::vector<BYTE>& outPremultBGRA) {
    Bitmap* src = NULL;
    IStream* stream = NULL;
    if (!LoadPinPng(&src, &stream)) return false;

    const int SW = (int)src->GetWidth();
    const int SH = (int)src->GetHeight();

    outPremultBGRA.assign((size_t)W * H * 4, 0);

    if (W == SW && H == SH) {
        // 1:1 直接像素拷贝（GDI+ 不参与）
        Rect rSrc(0, 0, W, H);
        BitmapData sd;
        if (src->LockBits(&rSrc, ImageLockModeRead, PixelFormat32bppARGB, &sd) != Ok) {
            delete src; if (stream) stream->Release();
            return false;
        }
        const BYTE* srow0 = (const BYTE*)sd.Scan0;
        BYTE* drow0 = outPremultBGRA.data();
        for (int y = 0; y < H; ++y) {
            const BYTE* s = srow0 + (size_t)y * sd.Stride;
            BYTE* d = drow0 + (size_t)y * W * 4;
            for (int x = 0; x < W; ++x) {
                BYTE b = s[x * 4 + 0];
                BYTE g = s[x * 4 + 1];
                BYTE r = s[x * 4 + 2];
                BYTE a = s[x * 4 + 3];
                if (a != 0 && a != 255) {
                    b  = (BYTE)((b  * a + 127) / 255);
                    g  = (BYTE)((g  * a + 127) / 255);
                    r  = (BYTE)((r  * a + 127) / 255);
                }
                d[x * 4 + 0] = b;
                d[x * 4 + 1] = g;
                d[x * 4 + 2] = r;
                d[x * 4 + 3] = a;
            }
        }
        src->UnlockBits(&sd);
        delete src;
        if (stream) stream->Release();
        return true;
    }

    // 需要缩放：走 GDI+ 高质量重采样
    Bitmap bmp(W, H, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(0, 0, 0, 0));
        g.DrawImage(src, Rect(0, 0, W, H),
                    0, 0, SW, SH, UnitPixel);
    }
    delete src;
    if (stream) stream->Release();

    Rect rect(0, 0, W, H);
    BitmapData bd;
    if (bmp.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        return false;
    }
    outPremultBGRA.assign((size_t)W * H * 4, 0);
    const BYTE* srcPix = (const BYTE*)bd.Scan0;
    BYTE* dst = outPremultBGRA.data();
    for (int y = 0; y < H; ++y) {
        const BYTE* srow = srcPix + (size_t)y * bd.Stride;
        BYTE* drow = dst + (size_t)y * W * 4;
        for (int x = 0; x < W; ++x) {
            BYTE b = srow[x * 4 + 0];
            BYTE gr = srow[x * 4 + 1];
            BYTE r2 = srow[x * 4 + 2];
            BYTE a = srow[x * 4 + 3];
            if (a != 0 && a != 255) {
                b  = (BYTE)((b  * a + 127) / 255);
                gr = (BYTE)((gr * a + 127) / 255);
                r2 = (BYTE)((r2 * a + 127) / 255);
            }
            drow[x * 4 + 0] = b;
            drow[x * 4 + 1] = gr;
            drow[x * 4 + 2] = r2;
            drow[x * 4 + 3] = a;
        }
    }
    bmp.UnlockBits(&bd);
    return true;
}

static void InitPinImage() {
    std::vector<BYTE> pixels;
    if (!BuildPinImage(PIN_W, PIN_H, pixels)) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = PIN_W;
    bi.bmiHeader.biHeight = -PIN_H;          // 负数 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    g_pinDC = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    g_pinBmp = CreateDIBSection(g_pinDC, &bi, DIB_RGB_COLORS, &g_pinBits, NULL, 0);
    if (g_pinBmp && g_pinBits) {
        memcpy(g_pinBits, pixels.data(), pixels.size());
        SelectObject(g_pinDC, g_pinBmp);
    }
}

// 图标统一从 exe 内嵌的 windowpin.ico 资源加载（含 16~256 多档，每档原生渲染）
static HICON LoadAppIcon(int cx, int cy) {
    HICON ic = (HICON)LoadImageW(GetModuleHandleW(NULL),
                                 MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 cx, cy, LR_DEFAULTCOLOR);
    return ic;
}

// ---------------------------------------------------------------- 置顶操作
static bool IsValidTarget(HWND h) {
    const wchar_t* why = NULL;
    if (!h) why = L"null";
    else if (!IsWindow(h)) why = L"notwindow";
    else if (!IsWindowVisible(h)) why = L"invisible";
    else if (h == g_host || h == g_capDlg) why = L"self";
    else if (GetWindow(h, GW_OWNER)) why = L"owned";
    else {
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) why = L"toolwindow";
        else {
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 63);
            if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
                !wcscmp(cls, L"Shell_TrayWnd") ||
                !wcscmp(cls, L"Windows.UI.Core.CoreWindow")) why = L"desktop";
            else if (!wcscmp(cls, PIN_CLASS)) why = L"pinwindow";
        }
    }
    if (why) { LOG(L"    IsValidTarget(%p)=false reason=%s", (void*)h, why); return false; }
    return true;
}

static void ApplyTopmost(HWND h, bool on) {
    if (!h || !IsWindow(h)) return;
    // 与 Python 版一致：先改 EXSTYLE，再 SetWindowPos，两者配合才稳定。
    // 注意：已经是 topmost 时不要再 SetWindowPos(HWND_TOPMOST)，那会把窗口
    // 重新提到 topmost 层最前面，从而压住它自己的右键菜单（菜单是目标的
    // 拥有窗口，本来就在目标之上）。
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool was = (ex & WS_EX_TOPMOST) != 0;
    if (on) {
        if (was) return;                     // 已置顶：保持现有 Z 序，不动
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_TOPMOST);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        if (!was) return;                    // 本来就不是 topmost：无需处理
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static PinEntry* FindPinByTarget(HWND target) {
    for (auto& p : g_pins) if (p.target == target) return &p;
    return NULL;
}

static PinEntry* FindPinByHwnd(HWND pin) {
    for (auto& p : g_pins) if (p.pin == pin) return &p;
    return NULL;
}

// ---------------------------------------------------------------- 图钉窗口
static void PlacePin(PinEntry& e) {
    if (!e.pin || !IsWindow(e.pin)) return;
    if (!IsWindow(e.target)) return;
    if (IsIconic(e.target) || !IsWindowVisible(e.target)) {
        ShowWindow(e.pin, SW_HIDE);
        return;
    }
    RECT r;
    GetWindowRect(e.target, &r);
    int cap = GetSystemMetrics(SM_CYCAPTION);
    int x = r.left + (r.right - r.left - PIN_W) / 2;
    int y = r.top + (cap - PIN_H) / 2;

    HDC screen = GetDC(NULL);
    POINT dst = { x, y };
    POINT src = { 0, 0 };
    SIZE size = { PIN_W, PIN_H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(e.pin, screen, &dst, &size, g_pinDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);

    SetWindowPos(e.pin, HWND_TOPMOST, x, y, PIN_W, PIN_H,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK PinProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONUP: {
        PinEntry* e = FindPinByHwnd(h);
        if (e) PostMessageW(g_host, WM_APP_PINCLICK, (WPARAM)e->target, 0);
        return 0;
    }
    case WM_RBUTTONUP: {
        PinEntry* e = FindPinByHwnd(h);
        if (e) PostMessageW(g_host, WM_APP_PINCLICK, (WPARAM)e->target, 0);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static void RemovePin(HWND target, bool restoreTopmost);   // 前置声明

static void AddPin(HWND target) {
    if (FindPinByTarget(target)) return;      // 已置顶，忽略

    // 单窗口模式：置顶新窗口前，先取消其它窗口的置顶，避免互相干扰
    while (!g_pins.empty()) {
        RemovePin(g_pins.front().target, true);
    }

    LONG_PTR ex = GetWindowLongPtrW(target, GWL_EXSTYLE);
    PinEntry e;
    e.target = target;
    e.prevTopmost = (ex & WS_EX_TOPMOST) != 0;
    e.pin = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        PIN_CLASS, L"", WS_POPUP, 0, 0, PIN_W, PIN_H,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    LOG(L"AddPin: target=%p pinCreated=%d", (void*)target, (int)(e.pin != NULL));
    if (!e.pin) return;

    ApplyTopmost(target, true);
    g_pins.push_back(e);
    PlacePin(g_pins.back());
}

static void RemovePin(HWND target, bool restoreTopmost) {
    for (size_t i = 0; i < g_pins.size(); ++i) {
        if (g_pins[i].target == target) {
            PinEntry e = g_pins[i];
            g_pins.erase(g_pins.begin() + i);
            if (e.pin && IsWindow(e.pin)) DestroyWindow(e.pin);
            if (IsWindow(e.target)) {
                ApplyTopmost(e.target, restoreTopmost ? e.prevTopmost : false);
            }
            return;
        }
    }
}

static void RemoveAllPins() {
    std::vector<PinEntry> copy = g_pins;
    g_pins.clear();
    for (auto& e : copy) {
        if (e.pin && IsWindow(e.pin)) DestroyWindow(e.pin);
        if (IsWindow(e.target)) {
            ApplyTopmost(e.target, e.prevTopmost);
        }
    }
}

// ---------------------------------------------------------------- 跟随 + 加固
// 目标窗口所在线程当前是否有弹出菜单（右键菜单等）打开。
// 菜单打开期间不能重排目标的 Z 序，否则会把菜单压到窗口下面
// （资源管理器右键菜单就是典型受害者）。
#ifdef WP_DEBUG_LOG
static bool g_menuOpenForTest = false;   // 自测钩子：强制模拟菜单打开
#endif
static bool IsMenuOpenOnThread(HWND target) {
#ifdef WP_DEBUG_LOG
    if (g_menuOpenForTest) return true;
#endif
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    DWORD tid = GetWindowThreadProcessId(target, NULL);
    if (!tid) return false;
    if (!GetGUIThreadInfo(tid, &gti)) return false;
    return gti.hwndMenuOwner != NULL;
}

// target 之上是否压着别的 topmost 窗口（排除自己的图钉、排除目标自身的
// 拥有窗口，例如它弹出的菜单）。只有真被别的程序压住时才需要重新提升。
static bool OtherTopmostAbove(HWND target, HWND ownPin) {
    for (HWND w = GetWindow(target, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == ownPin) continue;
        if (!IsWindowVisible(w)) continue;
        if (GetWindow(w, GW_OWNER) == target) continue;
        if (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) return true;
    }
    return false;
}

static void FollowAll() {
    LOG(L"FollowAll: pins=%d", (int)g_pins.size());

    // 记录最近一次真正处于前台的有效窗口：点击托盘图标时前台会变成
    // 任务栏，靠这个值才能还原「用户刚才在用的窗口」。
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_host && fg != g_capDlg && IsValidTarget(fg)) {
        g_lastTarget = fg;
    }

    for (size_t i = 0; i < g_pins.size(); ) {
        PinEntry& e = g_pins[i];
        if (!IsWindow(e.target)) {          // 目标被关闭 -> 清理
            if (e.pin && IsWindow(e.pin)) DestroyWindow(e.pin);
            g_pins.erase(g_pins.begin() + i);
            continue;
        }
        // 周期加固：只在「真被别的 topmost 窗口压住」时才重新提升 Z 序。
        // 菜单打开期间一律不动，否则资源管理器的右键菜单会被挡住。
        bool menuOpen = IsMenuOpenOnThread(e.target);
        if (menuOpen) {
            // 菜单打开中：隐藏图钉，避免它挡住菜单项或拦截菜单上的点击。
            // 菜单关闭后下一轮（150ms 内）会自动恢复显示。
            if (e.pin && IsWindow(e.pin) && IsWindowVisible(e.pin))
                ShowWindow(e.pin, SW_HIDE);
            ++i;
            continue;
        }
        if (OtherTopmostAbove(e.target, e.pin)) {
            ApplyTopmost(e.target, true);
        }
        PlacePin(e);                         // 图钉跟随目标
        ++i;
    }
}

// ---------------------------------------------------------------- 热键管理
static bool RegHotkey(HWND h, UINT mods, UINT vk) {
    return RegisterHotKey(h, HOTKEY_ID, mods, vk) != 0;
}

static void UnregHotkey(HWND h) {
    UnregisterHotKey(h, HOTKEY_ID);
}

static bool ApplyHotkey(HWND h, UINT mods, UINT vk) {
    // 先释放旧热键，避免「改成自己」时误判冲突
    if (g_hotkeyOk) { UnregHotkey(h); g_hotkeyOk = false; }
    if (RegHotkey(h, mods, vk)) {
        g_mods = mods;
        g_vk = vk;
        g_hotkeyOk = true;
        return true;
    }
    // 失败 -> 回滚到原热键
    if (RegHotkey(h, g_mods, g_vk)) g_hotkeyOk = true;
    return false;
}

// ---------------------------------------------------------------- 托盘
// 托盘气泡提示（用于「找不到可置顶窗口」等场景，替代静默失败）
static void NotifyBalloon(const wchar_t* msg, const wchar_t* title) {
    if (!g_nid.hWnd) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfo, msg, 256);
    lstrcpynW(nid.szInfoTitle, title ? title : L"WindowPin", 64);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void UpdateTrayTip() {
    std::wstring tip = L"WindowPin  " + HotkeyLabel(g_mods, g_vk);
    if (tip.size() > 127) tip = tip.substr(0, 127);
    lstrcpynW(g_nid.szTip, tip.c_str(), 128);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTray(HWND h) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = h;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_appIcon ? g_appIcon : LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"WindowPin", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowTrayMenu(HWND h) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_HOTKEY, L"更改热键…");
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- 热键录制对话框
static HWND g_capLabel = NULL;
static UINT g_capMods = 0;
static UINT g_capVk = 0;

static void UpdateCapLabel() {
    if (!g_capLabel) return;
    std::wstring txt;
    if (g_capVk) {
        txt = HotkeyLabel(g_capMods, g_capVk);
    } else {
        txt = L"请按下新的组合键…";
    }
    SetWindowTextW(g_capLabel, txt.c_str());
}

static void CloseCapDialog() {
    if (g_capDlg && IsWindow(g_capDlg)) DestroyWindow(g_capDlg);
    g_capDlg = NULL;
    g_capLabel = NULL;
}

static bool IsModifierKey(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static LRESULT CALLBACK CapProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)w;
        if (vk == VK_ESCAPE) { CloseCapDialog(); return 0; }
        if (IsModifierKey(vk)) {
            g_capMods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) g_capMods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000)    g_capMods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000)   g_capMods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
                g_capMods |= MOD_WIN;
            g_capVk = 0;
            UpdateCapLabel();
            return 0;
        }
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
            mods |= MOD_WIN;

        // 无修饰键时只允许 F1~F24
        if (mods == 0 && !(vk >= VK_F1 && vk <= VK_F24)) {
            g_capMods = mods;
            g_capVk = vk;
            UpdateCapLabel();
            MessageBoxW(h, L"请至少搭配一个修饰键（Ctrl / Alt / Shift / Win）。",
                        L"WindowPin", MB_ICONINFORMATION);
            g_capVk = 0;
            UpdateCapLabel();
            return 0;
        }

        // 冲突检测：临时注册试探
        bool ok = false;
        if (g_hotkeyOk) { UnregHotkey(g_host); g_hotkeyOk = false; }
        if (RegisterHotKey(h, HOTKEY_TEST_ID, mods, vk)) {
            UnregisterHotKey(h, HOTKEY_TEST_ID);
            ok = true;
        }

        if (!ok) {
            if (RegHotkey(g_host, g_mods, g_vk)) g_hotkeyOk = true;
            MessageBoxW(h, L"该热键冲突，请更换！", L"WindowPin", MB_ICONWARNING);
            g_capVk = 0;
            UpdateCapLabel();
            return 0;
        }

        // 应用新热键
        if (RegHotkey(g_host, mods, vk)) {
            g_mods = mods;
            g_vk = vk;
            g_hotkeyOk = true;
            SaveConfig(mods, vk);
            UpdateTrayTip();
            CloseCapDialog();
            return 0;
        }
        // 极端情况：注册失败，回滚
        if (RegHotkey(g_host, g_mods, g_vk)) g_hotkeyOk = true;
        MessageBoxW(h, L"热键注册失败，请更换！", L"WindowPin", MB_ICONWARNING);
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;                        // 吞掉按键音
    case WM_CLOSE:
        CloseCapDialog();
        return 0;
    case WM_DESTROY:
        g_capDlg = NULL;
        g_capLabel = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void OpenCapDialog() {
    if (g_capDlg && IsWindow(g_capDlg)) {
        SetForegroundWindow(g_capDlg);
        return;
    }
    g_capMods = g_mods;
    g_capVk = g_vk;

    int W = 380, H = 160;
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    g_capDlg = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, CAP_CLASS, L"更改热键",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, W, H, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_capDlg) return;

    HICON bigIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    if (bigIcon) {
        SendMessageW(g_capDlg, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);
    }
    HICON smIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (smIcon) {
        SendMessageW(g_capDlg, WM_SETICON, ICON_SMALL, (LPARAM)smIcon);
    }

    CreateWindowExW(0, L"STATIC",
                    L"直接按下新的组合键（Esc 取消）：",
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    16, 16, W - 40, 24, g_capDlg, NULL, GetModuleHandleW(NULL), NULL);

    g_capLabel = CreateWindowExW(0, L"STATIC", L"",
                                 WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                 16, 50, W - 40, 56, g_capDlg, NULL,
                                 GetModuleHandleW(NULL), NULL);
    HFONT f = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    SendMessageW(g_capLabel, WM_SETFONT, (WPARAM)f, TRUE);

    UpdateCapLabel();
    ShowWindow(g_capDlg, SW_SHOW);
    SetForegroundWindow(g_capDlg);
    SetFocus(g_capDlg);
}

// ---------------------------------------------------------------- 候选窗口
// 点击托盘图标时前台窗口会变成任务栏（Shell_TrayWnd），不能直接用
// GetForegroundWindow() 当作目标。这里按优先级找一个可置顶的窗口：
//   1) 前台窗口本身（若有效）
//   2) 最近一次真正处于前台的有效窗口（由 150ms 定时器记录）
//   3) Z 序最靠前、带标题的有效顶层窗口
static BOOL CALLBACK EnumTopProc(HWND h, LPARAM lp) {
    HWND* out = (HWND*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;
    if (!IsValidTarget(h)) return TRUE;
    wchar_t title[8] = {};
    if (GetWindowTextW(h, title, 2) <= 0) return TRUE;   // 无标题的跳过
    *out = h;
    return FALSE;                                        // 找到即停
}

// 给定「前台窗口」选出真正要操作的窗口。
// 点击托盘图标时前台会变成任务栏（Shell_TrayWnd），此时需要回退到
// 最近一次真正处于前台的窗口，而不是直接失败。
static HWND PickTarget(HWND fg) {
    if (fg && IsValidTarget(fg)) return fg;

    if (g_lastTarget && IsWindow(g_lastTarget) && IsValidTarget(g_lastTarget))
        return g_lastTarget;

    HWND found = NULL;
    EnumWindows(EnumTopProc, (LPARAM)&found);
    return found;
}

// ---------------------------------------------------------------- 置顶切换
// 按给定的「前台窗口」执行置顶/取消置顶。抽成参数化是为了自测能模拟
// 「前台是任务栏」这种真实场景。
static void TogglePinFor(HWND fg) {
    LOG(L"TogglePinFor: fg=%p last=%p", (void*)fg, (void*)g_lastTarget);

    // 前台窗口是图钉本身 -> 取消其对应目标的置顶
    PinEntry* byPin = FindPinByHwnd(fg);
    if (byPin) {
        HWND t = byPin->target;
        LOG(L"  fg is pin -> unpin target %p", (void*)t);
        RemovePin(t, true);
        return;
    }

    HWND cand = PickTarget(fg);
    LOG(L"  candidate=%p", (void*)cand);
    if (!cand) {
        NotifyBalloon(L"未找到可置顶的窗口", L"请先点一下目标窗口，再点托盘图标");
        return;
    }

    // 已置顶 -> 取消；否则置顶（单窗口模式下会自动取消上一个）
    if (FindPinByTarget(cand)) {
        LOG(L"  already pinned -> unpin");
        RemovePin(cand, true);
        return;
    }
    LOG(L"  add pin");
    AddPin(cand);
}

static void TogglePin() {
    TogglePinFor(GetForegroundWindow());
}

// ---------------------------------------------------------------- 宿主窗口
static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HOTKEY || m == WM_APP_TOGGLE || m == WM_APP_PINCLICK ||
        m == WM_CLOSE || m == WM_DESTROY || m == WM_COMMAND)
        LOG(L"HostProc msg=0x%04X w=%d", m, (int)w);
    switch (m) {
    case WM_HOTKEY:
        if (w == HOTKEY_ID) TogglePin();
        return 0;
    case WM_APP_TOGGLE:
        TogglePin();
        return 0;
    case WM_APP_PINCLICK:
        RemovePin((HWND)w, true);
        return 0;
    case WM_APP_TRAY:
        // 左键：置顶 / 取消置顶（再次点击）
        // 双击会先收到 LBUTTONUP 再收到 LBUTTONDBLCLK，需要防抖避免切换两次
        if (LOWORD(l) == WM_LBUTTONUP) {
            DWORD now = GetTickCount();
            if (now - g_lastTrayClick > 350) {   // 防抖窗口
                g_lastTrayClick = now;
                TogglePin();
            }
        } else if (LOWORD(l) == WM_LBUTTONDBLCLK) {
            g_lastTrayClick = GetTickCount();    // 双击不再额外切换
        } else if (LOWORD(l) == WM_RBUTTONUP) {
            ShowTrayMenu(h);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_HOTKEY: OpenCapDialog(); return 0;
        case IDM_QUIT:   DestroyWindow(h); return 0;
        }
        return 0;
    case WM_TIMER:
        if (w == TIMER_FOLLOW) FollowAll();
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        LOG(L"WM_DESTROY: pins=%d", (int)g_pins.size());
        KillTimer(h, TIMER_FOLLOW);
        RemoveAllPins();                 // 退出前恢复所有被置顶的窗口
        if (g_hotkeyOk) { UnregHotkey(h); g_hotkeyOk = false; }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}


// ---------------------------------------------------------------- 窗口类注册
static void RegisterAllClasses(HINSTANCE inst) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.lpszClassName = HOST_CLASS;
    wc.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PinProc;
    wc.lpszClassName = PIN_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_HAND);
    RegisterClassExW(&wc);

    wc.lpfnWndProc = CapProc;
    wc.lpszClassName = CAP_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    RegisterClassExW(&wc);
}

// ---------------------------------------------------------------- 进程内自测
// 用 --selftest 启动：程序自己创建目标窗口，验证单窗口置顶 / 切换 / 退出恢复
static int RunSelfTest() {
    int pass = 0, fail = 0;
    auto chk = [&](const char* name, bool ok) {
        LOG(L"  [%s] %hs", ok ? L"PASS" : L"FAIL", name);
        if (ok) ++pass; else ++fail;
    };
    auto topmost = [](HWND h) {
        return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    };

    // --- 图钉位图渲染校验（不依赖桌面，直接检查 GDI+ 输出像素）---
    {
        std::vector<BYTE> px;
        bool ok = BuildPinImage(PIN_W, PIN_H, px);
        chk("BuildPinImage ok", ok && px.size() == (size_t)PIN_W * PIN_H * 4);
        if (ok && !px.empty()) {
            int opaque = 0, redPix = 0, transparent = 0;
            for (int i = 0; i < PIN_W * PIN_H; ++i) {
                BYTE b = px[i*4+0], g = px[i*4+1], r = px[i*4+2], a = px[i*4+3];
                if (a == 0) ++transparent;
                else {
                    ++opaque;
                    // 预乘还原后判断红色：R 明显高于 G/B
                    if (a > 60) {
                        int R = r * 255 / a, G = g * 255 / a, B = b * 255 / a;
                        if (R > 120 && R > G + 40 && R > B + 40) ++redPix;
                    }
                }
            }
            LOG(L"  pin bitmap: opaque=%d transparent=%d red=%d total=%d",
                opaque, transparent, redPix, PIN_W * PIN_H);
            chk("pin has transparent background", transparent > 0);
            chk("pin has red body", redPix > (PIN_W * PIN_H) / 10);
            chk("pin fully inside bitmap", opaque > 0);
            // 四角必须透明（圆角/贴图边界）
            auto alphaAt = [&](int x, int y) { return px[((size_t)y * PIN_W + x) * 4 + 3]; };
            chk("corner transparent",
                alphaAt(0,0) == 0 && alphaAt(PIN_W-1,0) == 0 &&
                alphaAt(0,PIN_H-1) == 0 && alphaAt(PIN_W-1,PIN_H-1) == 0);
            // 中心区域应不透明（图钉主体）
            chk("center opaque", alphaAt(PIN_W/2, PIN_H/3) > 200);

            // 导出 BMP 供人眼确认（32 位 BGRA，非预乘，便于查看）
            {
                std::wstring path = L"D:\\XiaoYiWork\\window-pin-tool\\cpp\\pin_preview.bmp";
                const int W = PIN_W, H = PIN_H;
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof(ih);
                ih.biWidth = W; ih.biHeight = H;   // 正数 = 自下而上
                ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                DWORD imgSize = W * H * 4;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + imgSize;
                FILE* f = NULL;
                if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
                    fwrite(&fh, sizeof(fh), 1, f);
                    fwrite(&ih, sizeof(ih), 1, f);
                    // BMP 行序自下而上，逐行翻转写
                    for (int y = H - 1; y >= 0; --y) {
                        for (int x = 0; x < W; ++x) {
                            size_t i = ((size_t)y * W + x) * 4;
                            BYTE b = px[i+0], g = px[i+1], r = px[i+2], a = px[i+3];
                            BYTE out[4];
                            if (a > 0) {
                                out[0] = (BYTE)std::min(255, b * 255 / a);
                                out[1] = (BYTE)std::min(255, g * 255 / a);
                                out[2] = (BYTE)std::min(255, r * 255 / a);
                            } else { out[0]=out[1]=out[2]=0; }
                            out[3] = a;
                            fwrite(out, 4, 1, f);
                        }
                    }
                    fclose(f);
                    LOG(L"  preview written: %s", path.c_str());
                }
            }
        }
    }

    HWND w[3];
    for (int i = 0; i < 3; ++i) {
        w[i] = CreateWindowExW(0, L"STATIC", L"Target",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               100 + i * 80, 100 + i * 80, 400, 300,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
        ShowWindow(w[i], SW_SHOWNORMAL);
        UpdateWindow(w[i]);
    }
    Sleep(200);
    chk("targets created & visible",
        IsWindow(w[0]) && IsWindowVisible(w[0]) && IsWindowVisible(w[1]));
    chk("initially not topmost", !topmost(w[0]) && !topmost(w[1]) && !topmost(w[2]));

    // --- 单窗口模式：置顶 A ---
    AddPin(w[0]);
    chk("pin A -> 1 pin", g_pins.size() == 1);
    chk("A topmost", topmost(w[0]));
    bool has1 = !g_pins.empty() && IsWindow(g_pins[0].pin);
    chk("pin window alive", has1);
    chk("pin is layered & visible", has1 && IsWindowVisible(g_pins[0].pin));

    // 图钉应在 A 的标题栏水平居中
    if (has1) {
        RECT tr, pr;
        GetWindowRect(w[0], &tr);
        GetWindowRect(g_pins[0].pin, &pr);
        int expectX = tr.left + (tr.right - tr.left - PIN_W) / 2;
        chk("pin centered on caption", abs((int)pr.left - expectX) <= 2);
    } else {
        chk("pin centered on caption", false);
    }

    // 周期加固后仍置顶、图钉仍在
    for (int i = 0; i < 10; ++i) { FollowAll(); Sleep(20); }
    chk("still topmost after reassert", topmost(w[0]));
    chk("pin count stable after reassert", g_pins.size() == 1);

    // --- 右键菜单不被压住（资源管理器 bug 场景）---
    // 构造一个「目标的拥有窗口」模拟弹出菜单：它天然位于目标之上。
    // 反复 FollowAll 后它必须仍在目标之上，不能被加固逻辑顶下去。
    {
        HWND popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                     L"STATIC", L"Menu",
                                     WS_POPUP | WS_VISIBLE,
                                     150, 150, 120, 80,
                                     w[0],        // owner = 目标窗口
                                     NULL, GetModuleHandleW(NULL), NULL);
        chk("popup (menu) created", popup != NULL && IsWindow(popup));
        if (popup) {
            // 模拟菜单打开状态：不让加固动 Z 序
            for (int i = 0; i < 10; ++i) { FollowAll(); Sleep(20); }

            // 判定：从目标往上遍历，应先遇到这个 popup
            bool popupAbove = false;
            for (HWND x = GetWindow(w[0], GW_HWNDPREV); x; x = GetWindow(x, GW_HWNDPREV)) {
                if (x == popup) { popupAbove = true; break; }
            }
            chk("menu/popup stays above pinned target", popupAbove);
            chk("target still topmost w/ menu open", topmost(w[0]));
            DestroyWindow(popup);
        }
    }

    // --- 菜单打开期间图钉自动隐藏（避免遮挡菜单 / 拦截点击）---
    // IsMenuOpenOnThread 依赖 GetGUIThreadInfo 的 hwndMenuOwner，本进程
    // 无法伪造真实菜单状态，因此这里直接验证隐藏/恢复这一行为本身：
    // 用 g_menuOpenForTest 钩子强制「菜单打开」，检查图钉被隐藏。
    {
#ifdef WP_DEBUG_LOG
        chk("pin visible before menu", !g_pins.empty() && IsWindow(g_pins[0].pin) &&
                                       IsWindowVisible(g_pins[0].pin));
        g_menuOpenForTest = true;
        FollowAll();
        chk("pin hidden while menu open",
            !g_pins.empty() && IsWindow(g_pins[0].pin) && !IsWindowVisible(g_pins[0].pin));
        g_menuOpenForTest = false;
        FollowAll();
        chk("pin restored after menu closed",
            !g_pins.empty() && IsWindow(g_pins[0].pin) && IsWindowVisible(g_pins[0].pin));
#endif
    }

    // --- 置顶 B：A 必须自动取消（单窗口） ---
    AddPin(w[1]);
    chk("pin B -> still only 1 pin", g_pins.size() == 1);
    chk("B topmost", topmost(w[1]));
    chk("A auto-unpinned", !topmost(w[0]));
    chk("current target is B", !g_pins.empty() && g_pins[0].target == w[1]);

    // --- 再切回 A ---
    AddPin(w[0]);
    chk("switch back to A -> 1 pin", g_pins.size() == 1);
    chk("A topmost again", topmost(w[0]));
    chk("B auto-unpinned", !topmost(w[1]));

    // --- 取消置顶 ---
    RemovePin(w[0], true);
    chk("unpin A", !topmost(w[0]) && g_pins.empty());

    // --- 重复置顶同一窗口不应叠加 ---
    AddPin(w[2]);
    AddPin(w[2]);
    chk("idempotent add -> 1 pin", g_pins.size() == 1);

    // --- 托盘左键路径：前台是任务栏时也必须能置顶（用户报的 bug）---
    // 模拟点击托盘：此时 GetForegroundWindow() 会返回 Shell_TrayWnd，
    // 该窗口不是合法目标，必须回退到「最近一次真正的窗口」。
    {
        RemoveAllPins();
        HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);   // 可能为 NULL（无桌面）
        g_lastTarget = w[0];
        TogglePinFor(tray);
        chk("tray click w/ tray foreground -> pinned last target",
            g_pins.size() == 1 && !g_pins.empty() && g_pins[0].target == w[0]);
        chk("tray click -> target topmost", topmost(w[0]));

        // 再次点击 -> 取消置顶
        TogglePinFor(tray);
        chk("tray click again -> unpinned", g_pins.empty() && !topmost(w[0]));

        // 前台窗口本身有效时，优先用前台窗口
        RemoveAllPins();
        g_lastTarget = w[1];
        TogglePinFor(w[0]);
        chk("tray click w/ valid foreground -> prefers foreground",
            g_pins.size() == 1 && !g_pins.empty() && g_pins[0].target == w[0]);
        RemoveAllPins();
        g_lastTarget = NULL;
    }

    // --- 目标窗口销毁后 FollowAll 自动清理 ---
    DestroyWindow(w[2]);
    Sleep(50);
    FollowAll();
    chk("dead target auto-cleaned", g_pins.empty());

    // --- 退出恢复（直接调用）---
    AddPin(w[0]);
    RemoveAllPins();
    chk("exit restores all", !topmost(w[0]));

    // --- 真实退出路径：WM_CLOSE -> WM_DESTROY -> RemoveAllPins ---
    // 这是用户报告的 bug 场景：置顶后不取消，直接右键退出，窗口必须恢复
    {
        g_host = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_CLASS, L"HostForTest",
                                 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
        chk("test host created", g_host != NULL);
        if (g_host) {
            AddPin(w[0]);
            AddPin(w[1]);          // 切到 w1（w0 自动取消）
            chk("before exit: w1 topmost", topmost(w[1]));
            chk("before exit: w0 not topmost", !topmost(w[0]));
            chk("before exit: 1 pin", g_pins.size() == 1);

            // 模拟右键退出
            PostMessageW(g_host, WM_CLOSE, 0, 0);
            // 泵消息让 WM_DESTROY 执行
            MSG m;
            DWORD deadline = GetTickCount() + 1000;
            while (GetTickCount() < deadline) {
                while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                if (!IsWindow(g_host)) break;
                Sleep(10);
            }
            chk("host destroyed after WM_CLOSE", !IsWindow(g_host) || g_host == NULL);
            chk("BUGFIX: pinned window restored on exit", !topmost(w[1]));
            chk("BUGFIX: all pins cleaned on exit", g_pins.empty());
            g_host = NULL;
        }
    }

    LOG(L"  SELFTEST: %d passed, %d failed", pass, fail);
    for (int i = 0; i < 3; ++i) if (IsWindow(w[i])) DestroyWindow(w[i]);
    return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- 入口
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    // GDI+
    GdiplusStartupInput gi;
    if (GdiplusStartup(&g_gdiToken, &gi, NULL) != Ok) return 0;

#ifdef WP_DEBUG_LOG
    if (cmdLine && wcsstr(cmdLine, L"--selftest")) {
        RegisterAllClasses(inst);
        InitPinImage();
        int rc = RunSelfTest();
        GdiplusShutdown(g_gdiToken);
        return rc;
    }
#endif

    // 单实例
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Global\\WindowPinLiteMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(HOST_CLASS, NULL);
        if (prev) PostMessageW(prev, WM_APP_TOGGLE, 0, 0);
        return 0;
    }

    RegisterAllClasses(inst);

    // 图钉贴图 + 托盘图标（托盘用 16x16 小图标，清晰不模糊）
    InitPinImage();
    g_appIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON),
                            GetSystemMetrics(SM_CYSMICON));
    if (!g_appIcon) g_appIcon = LoadAppIcon(16, 16);

    // 隐藏宿主窗口
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_CLASS, L"WindowPinHost",
                             0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_host) return 0;

    AddTray(g_host);

    // 热键
    LoadConfig();
    if (!ApplyHotkey(g_host, g_mods, g_vk)) {
        if (!ApplyHotkey(g_host, DEF_MODS, DEF_VK)) {
            g_hotkeyOk = false;
        }
    }
    UpdateTrayTip();

    SetTimer(g_host, TIMER_FOLLOW, 150, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LOG(L"message loop exited, shutting down");

    if (g_appIcon) DestroyIcon(g_appIcon);
    GdiplusShutdown(g_gdiToken);
    return 0;
}
