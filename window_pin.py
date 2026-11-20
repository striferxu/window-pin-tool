# -*- coding: utf-8 -*-
"""
窗口图钉 —— Windows 窗口置顶小工具

交互模型（一键切换）：
  * 按热键 / 左键点击托盘图标 → 直接把「当前窗口」置顶；
    若当前窗口已置顶 → 取消置顶。
  * 置顶期间在目标窗口标题栏水平居中显示一个红色小图钉（状态指示器）；
    点击该图钉 = 取消置顶；取消置顶后图钉立即消失。
  * 托盘图标右键菜单只有两项：更改热键… / 退出。

热键可自定义并持久化到 %APPDATA%\\WindowPin\\config.ini，冲突时明确提示。
仅用 Win32 API，无 DLL 注入，对任意程序安全可靠。
"""
import os
import sys
import ctypes
import threading
import configparser

import win32api
import win32con
import win32gui
import win32ui
from PIL import Image, ImageDraw

_usr32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32


def set_timer(hwnd, id_, elapse_ms):
    return _usr32.SetTimer(hwnd, id_, elapse_ms, None)


def kill_timer(hwnd, id_):
    return _usr32.KillTimer(hwnd, id_)


# ---------------------------------------------------------------- 常量
TOPMOST = -1
NOTOPMOST = -2
MOD_WIN = win32con.MOD_WIN

WM_APP_TOGGLE = win32con.WM_APP + 3       # 切换当前窗口置顶
WM_APP_PIN_CLICKED = win32con.WM_APP + 4  # wParam = 目标窗口 hwnd（图钉被点击）
WM_APP_SET_HOTKEY = win32con.WM_APP + 5
WM_APP_QUIT = win32con.WM_APP + 6
WM_APP_REMOVE_ALL = win32con.WM_APP + 7

HOTKEY_ID = 1
HOTKEY_TEST_ID = 9998
DEFAULT_MODS = win32con.MOD_CONTROL | win32con.MOD_ALT | win32con.MOD_SHIFT
DEFAULT_VK = ord('P')

PIN_W, PIN_H = 26, 22
PIN_EMOJI = u"\U0001F4CC"          # 📌
PIN_EMOJI_FONT = u"Segoe UI Emoji"
PIN_EMOJI_PX = 16
PIN_FOLLOW_MS = 150
REASSERT_EVERY = 6

HOST_CLASS = "PinToolHost"
PIN_CLASS = "PinToolPin"
CAP_CLASS = "PinToolCapture"

_g_pin_map = {}
_g_capture = None
_g_host = None


# ================================================================ 热键配置
def config_path():
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, "WindowPin", "config.ini")


def load_config():
    mods, vk = DEFAULT_MODS, DEFAULT_VK
    try:
        if os.path.exists(config_path()):
            p = configparser.ConfigParser()
            p.read(config_path(), encoding="utf-8")
            mods = int(p.get("hotkey", "mods"))
            vk = int(p.get("hotkey", "vk"))
    except Exception:
        pass
    return mods, vk


def save_config(mods, vk):
    try:
        p = configparser.ConfigParser()
        p["hotkey"] = {"mods": str(int(mods)), "vk": str(int(vk)),
                       "label": hotkey_label(mods, vk)}
        d = os.path.dirname(config_path())
        if not os.path.isdir(d):
            os.makedirs(d)
        with open(config_path(), "w", encoding="utf-8") as f:
            p.write(f)
    except Exception:
        pass


def vk_name(vk):
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
        return chr(vk)
    if win32con.VK_F1 <= vk <= win32con.VK_F24:
        return "F%d" % (vk - win32con.VK_F1 + 1)
    names = {0x08: "Backspace", 0x09: "Tab", 0x0D: "Enter", 0x1B: "Esc",
             0x20: "Space", 0x25: "Left", 0x26: "Up", 0x27: "Right",
             0x28: "Down", 0x2D: "Insert", 0x2E: "Delete",
             0x13: "Pause"}
    return names.get(vk, "VK0x%X" % vk)


def hotkey_label(mods, vk):
    parts = []
    if mods & win32con.MOD_CONTROL:
        parts.append("Ctrl")
    if mods & win32con.MOD_ALT:
        parts.append("Alt")
    if mods & win32con.MOD_SHIFT:
        parts.append("Shift")
    if mods & win32con.MOD_WIN:
        parts.append("Win")
    return "+".join(parts + [vk_name(vk)])


def _combo_valid(mods, vk):
    if mods:
        return True
    return win32con.VK_F1 <= vk <= win32con.VK_F24


# ================================================================ 工具函数
def _rgb(r, g, b):
    return win32api.RGB(r, g, b)


def is_valid_target(hwnd):
    if not hwnd or not win32gui.IsWindow(hwnd):
        return False
    if not win32gui.IsWindowVisible(hwnd):
        return False
    if win32gui.IsIconic(hwnd):
        return False
    if hwnd == win32gui.GetDesktopWindow():
        return False
    ex = win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE)
    if ex & win32con.WS_EX_TOOLWINDOW:
        return False
    cls = win32gui.GetClassName(hwnd)
    if cls in ("Progman", "WorkerW", "Shell_TrayWnd", "DV2ControlHost",
               "LockScreenAppWindow", HOST_CLASS, PIN_CLASS, CAP_CLASS):
        return False
    return True


def window_title(hwnd):
    try:
        return win32gui.GetWindowText(hwnd).strip()
    except Exception:
        return ""


def find_candidate_window():
    fg = win32gui.GetForegroundWindow()
    if is_valid_target(fg):
        return fg
    top = []

    def _cb(hwnd, lparam):
        top.append(hwnd)
        return True

    win32gui.EnumWindows(_cb, None)
    for hwnd in top:
        if is_valid_target(hwnd) and window_title(hwnd):
            return hwnd
    return None


def force_topmost(hwnd, on):
    """强置顶：设置 WS_EX_TOPMOST 扩展样式并调整 Z 序。"""
    try:
        ex = win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE)
        if on and not (ex & win32con.WS_EX_TOPMOST):
            win32gui.SetWindowLong(hwnd, win32con.GWL_EXSTYLE,
                                   ex | win32con.WS_EX_TOPMOST)
        elif not on and (ex & win32con.WS_EX_TOPMOST):
            win32gui.SetWindowLong(hwnd, win32con.GWL_EXSTYLE,
                                   ex & ~win32con.WS_EX_TOPMOST)
    except Exception:
        pass
    try:
        win32gui.SetWindowPos(
            hwnd, TOPMOST if on else NOTOPMOST, 0, 0, 0, 0,
            win32con.SWP_NOMOVE | win32con.SWP_NOSIZE | win32con.SWP_NOACTIVATE)
    except Exception:
        pass


def make_tray_image():
    """托盘图标：与标题栏图钉一致的 📌 彩色 Emoji。"""
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    try:
        from PIL import ImageFont
        font = ImageFont.truetype("C:/Windows/Fonts/seguiemj.ttf", 46)
        d.text((32, 32), PIN_EMOJI, font=font, embedded_color=True, anchor="mm")
    except Exception:
        d.ellipse([23, 7, 41, 25], fill=(226, 226, 226, 255), outline=(90, 90, 90, 255))
        d.polygon([(27, 21), (37, 21), (34, 56), (30, 56)],
                  fill=(226, 226, 226, 255), outline=(90, 90, 90, 255))
        d.ellipse([30, 10, 38, 18], fill=(232, 17, 35, 255))
    return img


# ---------------------------------------------------------------- 分层窗口（纯彩色 Emoji，无衬底）
class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", _BITMAPINFOHEADER), ("bmiColors", ctypes.c_uint32 * 3)]


class _BLENDFUNCTION(ctypes.Structure):
    _fields_ = [("BlendOp", ctypes.c_ubyte), ("BlendFlags", ctypes.c_ubyte),
                ("SourceConstantAlpha", ctypes.c_ubyte), ("AlphaFormat", ctypes.c_ubyte)]


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32)]


class _SIZE(ctypes.Structure):
    _fields_ = [("cx", ctypes.c_int32), ("cy", ctypes.c_int32)]


def render_emoji_rgba(size, px=None, emoji=None, font_path=None):
    """用 PIL + Segoe UI Emoji 渲染彩色 Emoji 到 RGBA 图（透明背景）。"""
    from PIL import ImageFont
    W, H = size
    px = px or max(12, int(min(W, H) * 0.86))
    emoji = emoji or PIN_EMOJI
    font_path = font_path or "C:/Windows/Fonts/seguiemj.ttf"
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(font_path, px)
        d.text((W // 2, H // 2), emoji, font=font, embedded_color=True, anchor="mm")
    except Exception:
        # 兜底手绘图钉
        cx = W // 2
        d.ellipse([cx - W // 4, 2, cx + W // 4, H // 2], fill=(232, 17, 35, 255))
        d.polygon([(cx - W // 6, H // 2), (cx + W // 6, H // 2),
                   (cx + 2, H - 3), (cx - 2, H - 3)], fill=(232, 17, 35, 255))
    return img


def _rgba_to_premult_bgra(img):
    """RGBA -> 预乘 alpha 的 BGRA 字节串（供 UpdateLayeredWindow）。"""
    W, H = img.size
    src = img.tobytes()
    buf = bytearray(W * H * 4)
    j = 0
    for i in range(0, len(src), 4):
        r, g, b, a = src[i], src[i + 1], src[i + 2], src[i + 3]
        buf[j] = (b * a) // 255
        buf[j + 1] = (g * a) // 255
        buf[j + 2] = (r * a) // 255
        buf[j + 3] = a
        j += 4
    return bytes(buf)


def update_layered(hwnd, img, x, y):
    """把 RGBA 图以 alpha 混合方式绘制到分层窗口的 (x,y) 位置。"""
    W, H = img.size
    data = _rgba_to_premult_bgra(img)
    screen_dc = _usr32.GetDC(0)
    mem_dc = _gdi32.CreateCompatibleDC(screen_dc)
    bmi = _BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = W
    bmi.bmiHeader.biHeight = -H          # top-down
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = 0      # BI_RGB
    ppv = ctypes.c_void_p()
    hbmp = _gdi32.CreateDIBSection(screen_dc, ctypes.byref(bmi), 0,
                                   ctypes.byref(ppv), None, 0)
    ctypes.memmove(ppv, data, len(data))
    _gdi32.SelectObject(mem_dc, hbmp)
    bf = _BLENDFUNCTION(0, 0, 255, 1)    # AC_SRC_OVER / AC_SRC_ALPHA
    ok = _usr32.UpdateLayeredWindow(
        hwnd, screen_dc, ctypes.byref(_POINT(x, y)), ctypes.byref(_SIZE(W, H)),
        mem_dc, ctypes.byref(_POINT(0, 0)), 0, ctypes.byref(bf), 2)
    _gdi32.DeleteObject(hbmp)
    _gdi32.DeleteDC(mem_dc)
    _usr32.ReleaseDC(0, screen_dc)
    return bool(ok)
class PinWindow(object):
    """置顶期间显示的标题栏图钉；点击它 = 取消置顶。"""

    def __init__(self, target, on_remove, on_click):
        self.target = target
        self.on_remove = on_remove
        self.on_click = on_click
        self._ticks = 0
        self.hwnd = None
        self._create()

    def _pin_pos(self):
        rect = win32gui.GetWindowRect(self.target)
        cap = win32api.GetSystemMetrics(win32con.SM_CYCAPTION)
        x = rect[0] + (rect[2] - rect[0] - PIN_W) // 2
        y = rect[1] + max(0, (cap - PIN_H) // 2)
        return x, y

    def _create(self):
        self._img = render_emoji_rgba((PIN_W, PIN_H), px=PIN_EMOJI_PX)
        self.hwnd = win32gui.CreateWindowEx(
            win32con.WS_EX_LAYERED | win32con.WS_EX_TOPMOST
            | win32con.WS_EX_TOOLWINDOW | win32con.WS_EX_NOACTIVATE,
            PIN_CLASS, "window-pin", win32con.WS_POPUP,
            0, 0, PIN_W, PIN_H,
            None, None, win32api.GetModuleHandle(None), None)
        _g_pin_map[self.hwnd] = self
        self._render()
        win32gui.ShowWindow(self.hwnd, win32con.SW_SHOWNOACTIVATE)
        set_timer(self.hwnd, 1, PIN_FOLLOW_MS)

    def _render(self):
        """把缓存的彩色 Emoji 图绘制到分层窗口当前位置（透明背景，无衬底）。"""
        try:
            x, y = self._pin_pos()
            update_layered(self.hwnd, self._img, x, y)
        except Exception:
            pass

    def handle(self, hwnd, msg, wparam, lparam):
        if msg == win32con.WM_LBUTTONDOWN:
            self.on_click(self.target)
            return 0
        if msg == win32con.WM_TIMER:
            self._follow()
            return 0
        if msg == win32con.WM_DESTROY:
            kill_timer(hwnd, 1)
            _g_pin_map.pop(hwnd, None)
            self.on_remove(self.target)
            return 0
        # 分层窗口由 UpdateLayeredWindow 负责绘制，忽略 WM_PAINT 的擦除
        if msg == win32con.WM_ERASEBKGND:
            return 1
        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

    def _follow(self):
        self._ticks += 1
        if not win32gui.IsWindow(self.target):
            self._destroy()
            return
        if win32gui.IsIconic(self.target) or not win32gui.IsWindowVisible(self.target):
            win32gui.ShowWindow(self.hwnd, win32con.SW_HIDE)
            return
        if self._ticks % REASSERT_EVERY == 0:
            try:
                force_topmost(self.target, True)
            except Exception:
                pass
        self._render()                       # 位置变化时重绘（含移动跟随）
        if not win32gui.IsWindowVisible(self.hwnd):
            win32gui.ShowWindow(self.hwnd, win32con.SW_SHOWNOACTIVATE)
        self._ensure_on_top()                # 防止图钉被目标窗口盖住

    def _ensure_on_top(self):
        """若图钉被目标窗口（重申置顶或用户激活）压到下面，则重新抬到最顶层。"""
        try:
            x, y = self._pin_pos()
            cx = x + PIN_W // 2
            cy = y + PIN_H // 2
            if win32gui.WindowFromPoint((cx, cy)) != self.hwnd:
                win32gui.SetWindowPos(
                    self.hwnd, TOPMOST, 0, 0, 0, 0,
                    win32con.SWP_NOMOVE | win32con.SWP_NOSIZE
                    | win32con.SWP_NOACTIVATE)
        except Exception:
            pass

    def _destroy(self):
        if self.hwnd and win32gui.IsWindow(self.hwnd):
            win32gui.DestroyWindow(self.hwnd)


# ================================================================ 宿主窗口
class Host(object):
    def __init__(self):
        self.pins = {}          # target hwnd -> PinWindow
        self.hotkey = (DEFAULT_MODS, DEFAULT_VK, False)
        self.hwnd = self._create_hidden()

    def _create_hidden(self):
        return win32gui.CreateWindowEx(
            win32con.WS_EX_TOOLWINDOW, HOST_CLASS, "pin-tool-host", 0,
            0, 0, 0, 0, None, None, win32api.GetModuleHandle(None), None)

    # ---- 热键
    def _reg(self, hwnd, id_, mods, vk):
        # 注意：win32gui.RegisterHotKey 成功时返回 None，不能用 bool() 判断。
        # 直接调用 user32，返回值 != 0 才是成功。
        try:
            return bool(_usr32.RegisterHotKey(hwnd, id_, mods, vk))
        except Exception:
            return False

    def _unreg(self, hwnd, id_):
        try:
            _usr32.UnregisterHotKey(hwnd, id_)
        except Exception:
            pass

    def start_hotkey(self):
        mods, vk = load_config()
        if self._reg(self.hwnd, HOTKEY_ID, mods, vk):
            self.hotkey = (mods, vk, True)
            return hotkey_label(mods, vk), True
        self.hotkey = (mods, vk, False)
        return hotkey_label(mods, vk), False

    def apply_hotkey(self, mods, vk):
        """切换热键；冲突返回 False 且保留原热键。"""
        cur = self.hotkey
        if cur[2] and cur[0] == mods and cur[1] == vk:
            return True
        # 用临时 ID 探测该组合是否被其它程序占用
        if self._reg(self.hwnd, HOTKEY_TEST_ID, mods, vk):
            self._unreg(self.hwnd, HOTKEY_TEST_ID)
        else:
            return False                    # 冲突
        # 探测通过 -> 正式切换
        if cur[2]:
            self._unreg(self.hwnd, HOTKEY_ID)
            self.hotkey = (cur[0], cur[1], False)
        if self._reg(self.hwnd, HOTKEY_ID, mods, vk):
            self.hotkey = (mods, vk, True)
            save_config(mods, vk)
            return True
        return False

    # ---- 置顶切换
    def toggle_current(self):
        hwnd = find_candidate_window()
        if not hwnd:
            _notify("未找到可置顶的窗口，请先点击目标窗口再按热键", "窗口图钉")
            return
        if hwnd in self.pins:
            self.unpin(hwnd)
        else:
            self.pin(hwnd)

    def pin(self, hwnd):
        if hwnd in self.pins:
            return
        force_topmost(hwnd, True)
        self.pins[hwnd] = PinWindow(
            hwnd, self._pin_removed,
            lambda t: win32gui.PostMessage(self.hwnd, WM_APP_PIN_CLICKED, t, 0))
        _notify("已置顶：%s" % (window_title(hwnd) or hwnd), "窗口图钉")

    def unpin(self, hwnd):
        pin = self.pins.pop(hwnd, None)
        try:
            force_topmost(hwnd, False)
        except Exception:
            pass
        if pin is not None:
            pin._destroy()
        _notify("已取消置顶：%s" % (window_title(hwnd) or hwnd), "窗口图钉")

    def _pin_removed(self, target):
        self.pins.pop(target, None)

    def remove_all(self):
        for target in list(self.pins.keys()):
            pin = self.pins.pop(target, None)
            try:
                force_topmost(target, False)
            except Exception:
                pass
            if pin is not None:
                pin._destroy()

    def open_hotkey_dialog(self):
        global _g_capture
        if _g_capture is None or not win32gui.IsWindow(_g_capture.hwnd):
            _g_capture = CaptureDialog(self)

    def handle(self, hwnd, msg, wparam, lparam):
        if msg == win32con.WM_HOTKEY:
            if self.hotkey[2] and wparam == HOTKEY_ID:
                self.toggle_current()
            return 0
        if msg == WM_APP_TOGGLE:
            self.toggle_current()
            return 0
        if msg == WM_APP_PIN_CLICKED:
            self.unpin(wparam)
            return 0
        if msg == WM_APP_REMOVE_ALL:
            self.remove_all()
            return 0
        if msg == WM_APP_SET_HOTKEY:
            self.open_hotkey_dialog()
            return 0
        if msg == WM_APP_QUIT:
            self.remove_all()
            win32gui.PostQuitMessage(0)
            return 0
        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)


# ================================================================ 热键录制窗口
class CaptureDialog(object):
    def __init__(self, host):
        self.host = host
        self.message = (u"请按下新的热键组合（如 Ctrl+Alt+Shift+P）\n"
                        u"需至少包含一个 Ctrl / Alt / Shift / Win，或使用 F1-F24。\n"
                        u"按 Esc 取消。")
        self.hwnd = win32gui.CreateWindowEx(
            win32con.WS_EX_TOPMOST | win32con.WS_EX_TOOLWINDOW,
            CAP_CLASS, u"更改热键",
            win32con.WS_CAPTION | win32con.WS_SYSMENU,
            0, 0, 480, 160, None, None, win32api.GetModuleHandle(None), None)
        self._center()
        win32gui.ShowWindow(self.hwnd, win32con.SW_SHOW)
        try:
            win32gui.SetForegroundWindow(self.hwnd)
        except Exception:
            pass
        try:
            win32gui.SetFocus(self.hwnd)
        except Exception:
            pass

    def _center(self):
        wa = win32gui.GetWindowRect(win32gui.GetDesktopWindow())
        x = wa[0] + (wa[2] - wa[0] - 480) // 2
        y = wa[1] + (wa[3] - wa[1] - 160) // 2
        win32gui.SetWindowPos(self.hwnd, TOPMOST, x, y, 0, 0,
                              win32con.SWP_NOSIZE | win32con.SWP_NOACTIVATE)

    @staticmethod
    def _current_mods():
        def down(vk):
            return bool(win32api.GetAsyncKeyState(vk) & 0x8000)
        mods = 0
        if down(win32con.VK_CONTROL):
            mods |= win32con.MOD_CONTROL
        if down(win32con.VK_MENU):
            mods |= win32con.MOD_ALT
        if down(win32con.VK_SHIFT):
            mods |= win32con.MOD_SHIFT
        if down(0x5B) or down(0x5C):
            mods |= MOD_WIN
        return mods

    def handle(self, hwnd, msg, wparam, lparam):
        if msg in (win32con.WM_KEYDOWN, win32con.WM_SYSKEYDOWN):
            vk = wparam
            if vk == win32con.VK_ESCAPE:
                self._close()
                return 0
            if vk in (win32con.VK_CONTROL, win32con.VK_MENU,
                      win32con.VK_SHIFT, 0x5B, 0x5C):
                return 0
            mods = self._current_mods()
            if not _combo_valid(mods, vk):
                self.message = (u"该组合无效：请至少带一个 Ctrl / Alt / Shift / Win，\n"
                                u"或使用 F1-F24。请重新按键。")
                win32gui.InvalidateRect(hwnd, None, True)
                return 0
            if self.host.apply_hotkey(mods, vk):
                _notify(u"热键已更改为 %s" % hotkey_label(mods, vk), "窗口图钉")
                self._close()
            else:
                self.message = (u"该热键冲突，请更换！\n"
                                u"继续按下新的组合键，或按 Esc 取消。")
                win32gui.InvalidateRect(hwnd, None, True)
            return 0
        if msg == win32con.WM_PAINT:
            self._on_paint()
            return 0
        if msg == win32con.WM_DESTROY:
            global _g_capture
            _g_capture = None
            return 0
        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

    def _close(self):
        if self.hwnd and win32gui.IsWindow(self.hwnd):
            win32gui.DestroyWindow(self.hwnd)

    def _on_paint(self):
        hdc, ps = win32gui.BeginPaint(self.hwnd)
        try:
            lf = win32gui.LOGFONT()
            lf.lfHeight = -18
            lf.lfWeight = 400
            lf.lfCharSet = win32con.DEFAULT_CHARSET
            lf.lfQuality = win32con.DEFAULT_QUALITY
            lf.lfFaceName = u"Microsoft YaHei"
            hfont = win32gui.CreateFontIndirect(lf)
            oldfont = win32gui.SelectObject(hdc, hfont)
            win32gui.SetBkMode(hdc, win32con.TRANSPARENT)
            win32gui.SetTextColor(hdc, _rgb(30, 30, 30))
            rect = (12, 12, 468, 150)
            win32gui.DrawText(hdc, self.message, -1, rect,
                              win32con.DT_LEFT | win32con.DT_TOP | win32con.DT_WORDBREAK)
            win32gui.SelectObject(hdc, oldfont)
            win32gui.DeleteObject(hfont)
        finally:
            win32gui.EndPaint(self.hwnd, ps)


# ================================================================ 窗口过程
def _pin_wndproc(hwnd, msg, wparam, lparam):
    pin = _g_pin_map.get(hwnd)
    if pin is not None:
        return pin.handle(hwnd, msg, wparam, lparam)
    return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)


def _host_wndproc(hwnd, msg, wparam, lparam):
    if _g_host is not None:
        return _g_host.handle(hwnd, msg, wparam, lparam)
    return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)


def _cap_wndproc(hwnd, msg, wparam, lparam):
    cap = _g_capture
    if cap is not None and cap.hwnd == hwnd:
        return cap.handle(hwnd, msg, wparam, lparam)
    return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)


def register_classes():
    hinst = win32api.GetModuleHandle(None)
    hc = win32gui.WNDCLASS()
    hc.hInstance = hinst
    hc.lpszClassName = HOST_CLASS
    hc.lpfnWndProc = _host_wndproc
    win32gui.RegisterClass(hc)
    pc = win32gui.WNDCLASS()
    pc.hInstance = hinst
    pc.lpszClassName = PIN_CLASS
    pc.lpfnWndProc = _pin_wndproc
    pc.style = win32con.CS_HREDRAW | win32con.CS_VREDRAW
    win32gui.RegisterClass(pc)
    cc = win32gui.WNDCLASS()
    cc.hInstance = hinst
    cc.lpszClassName = CAP_CLASS
    cc.lpfnWndProc = _cap_wndproc
    cc.hbrBackground = win32con.COLOR_BTNFACE + 1
    win32gui.RegisterClass(cc)


# ================================================================ 托盘
TRAY = None


def _notify(text, title="窗口图钉"):
    try:
        if TRAY is not None:
            TRAY.notify(text, title)
    except Exception:
        pass


def _post_toggle(icon, item):
    try:
        win32gui.PostMessage(_g_host.hwnd, WM_APP_TOGGLE, 0, 0)
    except Exception:
        pass


def _post_set_hotkey(icon, item):
    try:
        win32gui.PostMessage(_g_host.hwnd, WM_APP_SET_HOTKEY, 0, 0)
    except Exception:
        pass


def _post_quit(icon, item):
    try:
        win32gui.PostMessage(_g_host.hwnd, WM_APP_QUIT, 0, 0)
    except Exception:
        pass


def build_tray():
    import pystray

    class ToggleIcon(pystray.Icon):
        """左键点击托盘图标 = 切换当前窗口置顶（不弹菜单）。"""
        def __call__(self):
            _post_toggle(self, None)

    menu = pystray.Menu(
        pystray.MenuItem("更改热键…", _post_set_hotkey),
        pystray.MenuItem("退出", _post_quit),
    )
    return ToggleIcon("WindowPinTool", make_tray_image(), "窗口图钉", menu)


# ================================================================ 主流程
def main():
    global _g_host, TRAY
    mutex = ctypes.windll.kernel32.CreateMutexW(None, False, "Global\\WindowPinToolMutex")
    if ctypes.windll.kernel32.GetLastError() == 183:
        try:
            win32gui.PostMessage(win32gui.FindWindow(HOST_CLASS, None),
                                 WM_APP_TOGGLE, 0, 0)
        except Exception:
            pass
        return 0

    register_classes()
    _g_host = Host()

    label, ok = _g_host.start_hotkey()

    TRAY = build_tray()
    threading.Thread(target=lambda: (TRAY.run(), None), daemon=True).start()
    if not ok:
        _notify("热键 %s 注册失败（可能被占用），请右键托盘图标选择“更改热键…”重设"
                % label, "窗口图钉")

    while True:
        msg = win32gui.GetMessage(None, 0, 0)
        mtuple = msg[1]
        if msg[0] == 0 or mtuple[1] == win32con.WM_QUIT:
            break
        win32gui.TranslateMessage(mtuple)
        win32gui.DispatchMessage(mtuple)

    try:
        TRAY.stop()
    except Exception:
        pass
    if _g_host.hotkey[2]:
        _g_host._unreg(_g_host.hwnd, HOTKEY_ID)
    return 0


def _quiet_excepthook(exc_type, exc_value, exc_tb):
    try:
        if TRAY is not None:
            TRAY.stop()
    except Exception:
        pass


if __name__ == "__main__":
    sys.excepthook = _quiet_excepthook
    try:
        rc = main()
        sys.exit(rc)
    except SystemExit:
        raise
    except Exception:
        _quiet_excepthook(*sys.exc_info())
        sys.exit(1)
