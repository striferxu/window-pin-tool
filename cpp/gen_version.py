# -*- coding: utf-8 -*-
"""从 VERSION.txt 生成 version.h（供 windowpin.rc 与 windowpin.cpp 使用）。

版本号唯一来源是 VERSION.txt，避免多处硬编码不一致。
运行：python gen_version.py [x64|x86]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    arch = sys.argv[1] if len(sys.argv) > 1 else ""
    ver_path = os.path.join(HERE, "VERSION.txt")
    raw = open(ver_path, encoding="utf-8").read().strip()
    parts = raw.split(".")
    if len(parts) != 3:
        raise SystemExit("VERSION.txt 必须是 MAJOR.MINOR.PATCH 格式，当前: %r" % raw)
    major, minor, patch = (int(p) for p in parts)

    full = "%d.%d.%d.0" % (major, minor, patch)          # 四段版本（VERSIONINFO 用）
    short = "%d.%d.%d" % (major, minor, patch)           # 三段版本（文件名用）

    if arch:
        filename = "WindowPinLite-v%s-%s.exe" % (short, arch)
    else:
        filename = "WindowPinLite-v%s.exe" % short

    out = os.path.join(HERE, "version.h")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("// 自动生成，请勿手工编辑（见 gen_version.py / VERSION.txt）\n")
        f.write("#pragma once\n\n")
        f.write("#define VER_MAJOR %d\n" % major)
        f.write("#define VER_MINOR %d\n" % minor)
        f.write("#define VER_PATCH %d\n" % patch)
        f.write("#define VER_BUILD 0\n\n")
        f.write('#define VER_STRING   "%s"\n' % full)
        f.write('#define VER_SHORT    "%s"\n' % short)
        f.write('#define VER_FILENAME "%s"\n' % filename)

    print("version.h -> %s (%s, %s)" % (out, full, filename))


if __name__ == "__main__":
    main()
