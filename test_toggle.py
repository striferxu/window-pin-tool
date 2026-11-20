# -*- coding: utf-8 -*-
"""自测：一键切换模型（pin/unpin/toggle）、图钉显示与消失、热键冲突检测。"""
import os
import sys
import time

import win32api
import win32con
import win32gui

import window_pin as wp

results = []


def check(name, cond, extra=""):
    results.append((name, cond, extra))
    print(("PASS " if cond else "FAIL ") + name + (" | " + extra if extra else ""))


def pump(seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        got = win32gui.PeekMessage(None, 0, 0, win32con.PM_REMOVE)
        if got[0]:
            win32gui.DispatchMessage(got[1])
        else:
            time.sleep(0.005)


def topmost(hwnd):
    return bool(win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE)
                & win32con.WS_EX_TOPMOST)


def main():
    wp.register_classes()
    host = wp.Host()
    wp._g_host = host

    target = win32gui.CreateWindow("STATIC", "Target", win32con.WS_OVERLAPPEDWINDOW,
                                   100, 100, 420, 300, None, None,
                                   win32api.GetModuleHandle(None), None)
    win32gui.ShowWindow(target, win32con.SW_SHOW)
    check("target created", win32gui.IsWindow(target))
    check("not topmost initially", not topmost(target))

    # pin -> 置顶 + 图钉出现
    host.pin(target)
    check("pin sets topmost", topmost(target))
    check("pin registered in host", target in host.pins)
    pin = host.pins[target]
    check("pin window exists", win32gui.IsWindow(pin.hwnd))
    check("pin visible", win32gui.IsWindowVisible(pin.hwnd))

    # 图钉位置在标题栏内且水平居中
    trect = win32gui.GetWindowRect(target)
    prect = win32gui.GetWindowRect(pin.hwnd)
    cap = win32api.GetSystemMetrics(win32con.SM_CYCAPTION)
    check("pin height within caption", (prect[3] - prect[1]) <= cap + 2,
          "pinH=%d cap=%d" % (prect[3] - prect[1], cap))
    tcx = (trect[0] + trect[2]) // 2
    pcx = (prect[0] + prect[2]) // 2
    check("pin horizontally centered", abs(tcx - pcx) <= 2, "tcx=%d pcx=%d" % (tcx, pcx))

    # unpin -> 取消置顶 + 图钉消失
    host.unpin(target)
    check("unpin clears topmost", not topmost(target))
    check("pin removed from host", target not in host.pins)
    check("pin window destroyed", not win32gui.IsWindow(pin.hwnd))

    # toggle：当前无图钉 -> 置顶
    wp.find_candidate_window = lambda: target     # 固定候选窗口
    host.toggle_current()
    check("toggle -> pin", target in host.pins and topmost(target))
    # 再 toggle -> 取消
    host.toggle_current()
    check("toggle -> unpin", target not in host.pins and not topmost(target))

    # 点击图钉消息 -> 取消置顶
    host.pin(target)
    win32gui.PostMessage(host.hwnd, wp.WM_APP_PIN_CLICKED, target, 0)
    pump(0.2)
    check("pin click unpins", target not in host.pins and not topmost(target))

    # 跟随：移动目标窗口
    host.pin(target)
    pin = host.pins[target]
    before = win32gui.GetWindowRect(pin.hwnd)
    win32gui.SetWindowPos(target, 0, 400, 300, 0, 0,
                          win32con.SWP_NOSIZE | win32con.SWP_NOZORDER)
    pump(0.6)
    after = win32gui.GetWindowRect(pin.hwnd)
    check("pin follows target", after[0] != before[0], "%s -> %s" % (before, after))

    # 回归：图钉在跨过周期性重申置顶后仍不被目标窗口遮挡（曾出现"约1秒后消失"的 bug）
    cx = (after[0] + after[2]) // 2
    cy = (after[1] + after[3]) // 2
    ok_start = win32gui.WindowFromPoint((cx, cy)) == pin.hwnd
    mismatches = 0
    deadline = time.time() + 1.6          # 跨过至少 2 次 REASSERT
    while time.time() < deadline:
        got = win32gui.PeekMessage(None, 0, 0, win32con.PM_REMOVE)
        if got[0]:
            win32gui.DispatchMessage(got[1])
        else:
            time.sleep(0.01)
        if win32gui.WindowFromPoint((cx, cy)) != pin.hwnd:
            mismatches += 1
    check("pin stays on top over time (no ~1s disappear)",
          ok_start and mismatches == 0 and win32gui.WindowFromPoint((cx, cy)) == pin.hwnd,
          "mismatches=%d" % mismatches)
    host.unpin(target)

    # 热键冲突检测（真实注册验证）
    mods = win32con.MOD_CONTROL | win32con.MOD_ALT
    # 1) 空闲组合可注册
    check("register free combo ok", host._reg(host.hwnd, 555, mods, ord('K')) is True)
    host._unreg(host.hwnd, 555)
    # 2) 真实切换到一个空闲组合 -> 成功
    check("apply_hotkey success on free combo", host.apply_hotkey(mods, ord('L')) is True)
    check("hotkey updated", host.hotkey[:2] == (mods, ord('L')) and host.hotkey[2] is True)
    # 3) 用独立窗口占住组合 -> apply_hotkey 判为冲突
    import ctypes
    u = ctypes.windll.user32
    h2 = win32gui.CreateWindowEx(win32con.WS_EX_TOOLWINDOW, "STATIC", "b", 0, 0, 0, 0, 0,
                                 None, None, win32api.GetModuleHandle(None), None)
    if u.RegisterHotKey(h2, 1, mods, ord('M')):
        conflict = host.apply_hotkey(mods, ord('M'))
        check("apply_hotkey detects real conflict", conflict is False)
        check("conflict keeps old hotkey", host.hotkey[:2] == (mods, ord('L')))
        u.UnregisterHotKey(h2, 1)
    else:
        check("apply_hotkey detects real conflict", False, "occupy failed in sandbox")
    win32gui.DestroyWindow(h2)
    # 清理配置
    try:
        os.remove(wp.config_path())
    except Exception:
        pass

    # 标签
    check("label", wp.hotkey_label(wp.MOD_WIN | win32con.MOD_SHIFT, ord('Q')) == "Shift+Win+Q",
          wp.hotkey_label(wp.MOD_WIN | win32con.MOD_SHIFT, ord('Q')))

    # 清理
    host.remove_all()
    win32gui.DestroyWindow(target)
    if win32gui.IsWindow(host.hwnd):
        win32gui.DestroyWindow(host.hwnd)

    failed = [r for r in results if not r[1]]
    print("---- %d/%d passed ----" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
