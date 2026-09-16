#!/usr/bin/env python3
"""Generate res/omtmini.ico without any third party imaging library.

The mark is a broadcast wave: a solid dot with two arcs radiating from it,
on the accent blue used throughout the interface. Rendered at 4x and box
filtered so the small sizes stay legible.
"""
import math
import os
import struct
import zlib

SS = 4  # supersample factor
SIZES = [16, 24, 32, 48, 64, 128, 256]

ACCENT_TOP = (0x60, 0xA5, 0xFA)
ACCENT_BOT = (0x25, 0x63, 0xEB)
WHITE = (0xFF, 0xFF, 0xFF)


def rounded_rect_alpha(x, y, w, h, radius):
    """Coverage of a rounded rect at point (x, y), 0.0 or 1.0 (supersampled)."""
    if radius <= 0:
        return 1.0
    cx = min(max(x, radius), w - radius)
    cy = min(max(y, radius), h - radius)
    dx, dy = x - cx, y - cy
    return 1.0 if (dx * dx + dy * dy) <= radius * radius else 0.0


def render(size):
    n = size * SS
    px = [[(0, 0, 0, 0)] * n for _ in range(n)]

    radius = n * 0.22
    cx, cy = n * 0.36, n * 0.68          # origin of the broadcast waves
    dot_r = n * 0.085

    # Arc bands, as (inner radius, outer radius)
    bands = [(n * 0.20, n * 0.265), (n * 0.35, n * 0.415)]
    a0, a1 = math.radians(-82.0), math.radians(-8.0)   # sweep up and to the right

    for j in range(n):
        for i in range(n):
            if not rounded_rect_alpha(i + 0.5, j + 0.5, n, n, radius):
                continue

            t = j / float(n - 1)
            bg = tuple(int(ACCENT_TOP[k] + (ACCENT_BOT[k] - ACCENT_TOP[k]) * t)
                       for k in range(3))
            r, g, b = bg

            dx, dy = (i + 0.5) - cx, (j + 0.5) - cy
            d = math.hypot(dx, dy)

            on_glyph = d <= dot_r
            if not on_glyph:
                ang = math.atan2(dy, dx)
                if a0 <= ang <= a1:
                    for lo, hi in bands:
                        if lo <= d <= hi:
                            on_glyph = True
                            break
            if on_glyph:
                r, g, b = WHITE

            px[j][i] = (r, g, b, 255)
    return px


def downsample(px, size):
    out = [[(0, 0, 0, 0)] * size for _ in range(size)]
    area = SS * SS
    for y in range(size):
        for x in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    pr, pg, pb, pa = px[y * SS + sy][x * SS + sx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            if a:
                out[y][x] = (r // a, g // a, b // a, a // area)
            else:
                out[y][x] = (0, 0, 0, 0)
    return out


def png_image(img, size):
    """PNG encoded entry, used for 256x256 so the file stays small."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)                      # filter type 0
        for x in range(size):
            r, g, b, a = img[y][x]
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def ico_image(img, size):
    """One BITMAPINFOHEADER + BGRA bottom-up pixels + AND mask."""
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        row = bytearray()
        for x in range(size):
            r, g, b, a = img[y][x]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    mask_stride = ((size + 31) // 32) * 4
    mask = b'\x00' * (mask_stride * size)
    return header + b''.join(rows) + mask


def main():
    out_path = os.path.join(os.path.dirname(__file__), '..', 'res', 'omtmini.ico')
    out_path = os.path.normpath(out_path)

    entries, blobs = [], []
    offset = 6 + 16 * len(SIZES)
    for size in SIZES:
        img = downsample(render(size), size)
        blob = png_image(img, size) if size >= 256 else ico_image(img, size)
        entries.append(struct.pack('<BBBBHHII',
                                   size if size < 256 else 0,
                                   size if size < 256 else 0,
                                   0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)

    with open(out_path, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(SIZES)))
        for e in entries:
            f.write(e)
        for b in blobs:
            f.write(b)
    print('wrote {} ({} bytes, {} sizes)'.format(out_path, offset, len(SIZES)))


if __name__ == '__main__':
    main()
