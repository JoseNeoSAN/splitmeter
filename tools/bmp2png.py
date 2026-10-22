"""把 SplitMeter 导出的预乘 BGRA 缓冲转成 PNG（反预乘 + 放大 + 棋盘底）。"""
import struct
import sys
import zlib


def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack("<I", d[10:14])[0]
    w = struct.unpack("<i", d[18:22])[0]
    h = struct.unpack("<i", d[22:26])[0]
    flip = h > 0
    h = abs(h)
    return w, h, d[off:off + w * h * 4], flip


def to_rgba_premultiplied_inverse(px, w, h, flip, crop_h=None, bg=(238, 240, 244)):
    """把预乘 BGRA 反预乘，再叠加到浅灰背景上（模拟悬浮在浅色桌面的观感）。"""
    out = bytearray()
    rows = min(h, crop_h) if crop_h else h
    for y in range(rows):
        sy = (h - 1 - y) if flip else y
        out.append(0)
        for x in range(w):
            i = (sy * w + x) * 4
            b, g, r, a = px[i], px[i + 1], px[i + 2], px[i + 3]
            if a == 0:
                out += bytes(bg)
                continue
            # 反预乘
            r2 = min(255, r * 255 // a)
            g2 = min(255, g * 255 // a)
            b2 = min(255, b * 255 // a)
            af = a / 255.0
            # PNG 是 RGB 顺序，必须按 R→G→B 写入；写反会让蓝变橙
            out.append(int(r2 * af + bg[0] * (1 - af)))
            out.append(int(g2 * af + bg[1] * (1 - af)))
            out.append(int(b2 * af + bg[2] * (1 - af)))
    return bytes(out), rows


def write_png(path, w, h, rgb_rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rgb_rows, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def scale_nearest(w, h, rows, factor):
    """rows 里每行前有 1 字节 filter。"""
    out = bytearray()
    for y in range(h):
        line = rows[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)]
        big = bytearray()
        for x in range(w):
            px = line[x * 3:x * 3 + 3]
            big += px * factor
        for _ in range(factor):
            out.append(0)
            out += big
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    crop = int(sys.argv[3]) if len(sys.argv) > 3 else None
    factor = int(sys.argv[4]) if len(sys.argv) > 4 else 3

    w, h, px, flip = read_bmp(src)
    rgb, rows = to_rgba_premultiplied_inverse(px, w, h, flip, crop)
    out = scale_nearest(w, rows, rgb, factor)
    write_png(dst, w * factor, rows * factor, out)
    print("已生成 %s  %dx%d" % (dst, w * factor, rows * factor))


if __name__ == "__main__":
    main()
