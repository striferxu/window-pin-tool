# -*- coding: utf-8 -*-
"""生成 WindowPinLite 的图标资源（由 Segoe UI Emoji U+1F4CC 渲染）。

产出：
  assets/pin_26x22.png    标题栏图钉贴图，与 Python 版逐像素一致（26x22 画布，16px 字体）
  assets/windowpin.ico    多尺寸图标（每档用原生字号渲染，PNG 压缩条目）
  pin_data.h              内嵌到 exe 的 PNG 字节数组
运行：python make_assets.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
FONT = "C:/Windows/Fonts/seguiemj.ttf"
EMOJI = "\U0001F4CC"          # 📌

# 与 window_pin.py 完全相同的参数
PIN_W, PIN_H = 26, 22
PIN_EMOJI_PX = 16


def render(size, font_px):
    """在 size=(W,H) 画布上居中绘制 emoji。"""
    W, H = size
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, font_px)
    d.text((W // 2, H // 2), EMOJI, font=f, embedded_color=True, anchor="mm")
    return img


# 各尺寸的绘制比例：小尺寸略收一点，避免 emoji 被画布边缘裁掉
ICON_SCALE = {16: 0.66, 20: 0.70, 24: 0.70}


def render_icon(side):
    """图标：按比例绘制，与 Python 版托盘图标（64 画布 / 46px 字体 ≈ 0.72）一致。"""
    scale = ICON_SCALE.get(side, 0.72)
    return render((side, side), max(6, int(round(side * scale))))


def _bmp_entry(img):
    """把 RGBA 图像编码成 ICO 内的传统 BMP(DIB) 帧。

    Windows 7 的图标加载器对小尺寸 PNG 压缩帧支持不好（常见现象是托盘 /
    exe 图标空白），因此 <=64px 一律用 BMP 帧，只有 128/256 用 PNG。
    BMP 帧结构：BITMAPINFOHEADER(biHeight = 2*h) + 32bpp BGRA(自下而上) + AND 掩码。
    """
    import struct
    w, h = img.size
    px = img.load()
    xor = bytearray()
    for y in range(h - 1, -1, -1):            # 自下而上
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))        # BGRA
    # AND 掩码：32bpp 有 alpha 时全 0 即可，但行需按 4 字节对齐
    mask_row = ((w + 31) // 32) * 4
    and_mask = bytes(mask_row * h)

    ih = struct.pack("<IiiHHIIiiII",
                     40,                 # biSize
                     w, h * 2,           # biWidth, biHeight(双倍)
                     1, 32,              # biPlanes, biBitCount
                     0, len(xor) + len(and_mask),   # biCompression=BI_RGB, biSizeImage
                     0, 0, 0, 0)
    return ih + bytes(xor) + and_mask


def build_ico(sizes, path):
    """手写 ICO：小尺寸用 BMP 帧（Win7 兼容），大尺寸用 PNG 帧。"""
    import struct, io
    entries = []
    for s in sizes:
        img = render_icon(s)
        if s <= 64:
            data = _bmp_entry(img)            # 传统格式，Win7 可靠
        else:
            buf = io.BytesIO()
            img.save(buf, format="PNG", optimize=True)
            data = buf.getvalue()             # Vista+ 才认 PNG 帧
        entries.append((s, data))

    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    dir_bytes, data_bytes = b"", b""
    for s, data in entries:
        w = 0 if s >= 256 else s
        dir_bytes += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), offset)
        data_bytes += data
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + dir_bytes + data_bytes)


def write_header(png_path, h_path, w, h):
    data = open(png_path, "rb").read()
    with open(h_path, "w", encoding="utf-8") as f:
        f.write("// 自动生成，请勿手工编辑（见 make_assets.py）\n")
        f.write("// Segoe UI Emoji U+1F4CC 渲染结果，%dx%d PNG\n" % (w, h))
        f.write("#pragma once\n")
        f.write("static const unsigned int PIN_PNG_SIZE = %d;\n" % len(data))
        f.write("static const unsigned char PIN_PNG[%d] = {\n" % len(data))
        for i in range(0, len(data), 20):
            f.write("    " + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
        f.write("};\n")


def main():
    os.makedirs(ASSETS, exist_ok=True)

    # 1) 标题栏图钉：与 Python 版逐像素一致（26x22 画布 / 16px 字体）
    pin = render((PIN_W, PIN_H), PIN_EMOJI_PX)
    pin_path = os.path.join(ASSETS, "pin_%dx%d.png" % (PIN_W, PIN_H))
    pin.save(pin_path, optimize=True)
    write_header(pin_path, os.path.join(HERE, "pin_data.h"), PIN_W, PIN_H)
    print("pin  -> %s (%d bytes)" % (pin_path, os.path.getsize(pin_path)))

    # 2) 多尺寸图标（每档原生渲染，避免缩放模糊）
    ico_path = os.path.join(ASSETS, "windowpin.ico")
    build_ico([16, 20, 24, 32, 40, 48, 64, 128, 256], ico_path)
    print("ico  -> %s (%d bytes)" % (ico_path, os.path.getsize(ico_path)))

    # 3) 预览图（人工确认用）
    render_icon(256).save(os.path.join(ASSETS, "preview_icon_256.png"))
    pin.resize((PIN_W * 8, PIN_H * 8), Image.NEAREST).save(
        os.path.join(ASSETS, "preview_pin_x8.png"))
    print("preview written")


if __name__ == "__main__":
    main()
