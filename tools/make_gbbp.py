#!/usr/bin/env python3
"""make_gbbp.py - a picture into a pico-gameboy backplane file (.gbbp).

Only the Python standard library: PNG (all colour types and bit depths,
interlaced too), GIF (the first frame), BMP (1/4/8/16/24/32 bits, uncompressed)
and PNM (PBM/PGM/PPM) are decoded here; other formats (JPEG, ...) are read
through Pillow when it is installed.

The backplane is made of cells: a cell is 2x2 pixels of the 640x480 screen
(a TV field line is a cell row).  One picture pixel per cell at scale 1:1.

    TV-SOFT  384 x 240 cells, cut from the scaled picture at (shift-x, shift-y);
    HDMI/VGA 320 x 240 cells, the same place 32 cells further right, so both
             show the picture at the same position on the screen.

Each of them has the 2x and the 3x variant (the game window in the middle:
160x144 or 240x216 cells); by default both are cut the same way, the 3x one
can get its own scale and shift (--scale-x3 ...).  The shift is counted in
the scaled picture (after --scale-x/--scale-y), so it means the same cells
for TV-SOFT and HDMI/VGA.

32 colours per part (k-means), every cell the nearest of them; --dither
spreads the error (Floyd-Steinberg) instead.  What lies outside the picture
and its transparent parts get the --fill colour.

File format (version 1, 338117 bytes):
    "GBBP", version byte;
    HDMI/VGA: 32 colours (R, G, B), 2x cells, 3x cells (320 x 240 colour numbers);
    TV-SOFT:  32 colours (R, G, B), 2x cells, 3x cells (384 x 240).

Examples:
    make_gbbp.py picture.png
    make_gbbp.py picture.png --scale-x 0.5128 --scale-y 0.5393 --shift-x 39.3 --shift-y 56.8 --preview p.png
"""

import argparse
import math
import os
import struct
import sys
import zlib

GBBP_VERSION = 1
COLORS = 32
ROWS = 240
PARTS = ((320, 32), (384, 0))  # (cells across, extra shift-x): HDMI/VGA, TV-SOFT
# the game window (x, y, w, h) in a part W cells wide
GAME = {2: lambda w: ((w - 160) // 2, 48, 160, 144), 3: lambda w: ((w - 240) // 2, 12, 240, 216)}


# ---------------------------------------------------------------- decoders
# Every decoder returns (width, height, rows) with rows of (r, g, b, a) tuples.

def _png(data):
    pos, chunks = 8, []
    while pos + 8 <= len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        chunks.append((kind, data[pos + 8:pos + 8 + length]))
        pos += 12 + length
    ihdr = dict(chunks)[b'IHDR']
    w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', ihdr)
    plte = dict(chunks).get(b'PLTE', b'')
    trns = dict(chunks).get(b'tRNS', None)
    raw = zlib.decompress(b''.join(body for kind, body in chunks if kind == b'IDAT'))
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bits = channels * depth
    bpp = max(1, bits // 8)
    maxv = (1 << depth) - 1
    out = [[None] * w for _ in range(h)]

    def unfilter(pw, ph, offset):
        stride = (pw * bits + 7) // 8
        rows, prev = [], bytearray(stride)
        for _ in range(ph):
            f = raw[offset]
            line = bytearray(raw[offset + 1:offset + 1 + stride])
            offset += 1 + stride
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                if f == 1:
                    line[i] = (line[i] + a) & 255
                elif f == 2:
                    line[i] = (line[i] + b) & 255
                elif f == 3:
                    line[i] = (line[i] + ((a + b) >> 1)) & 255
                elif f == 4:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            rows.append(line)
            prev = line
        return rows, offset

    def samples(line, count):
        if depth == 8:
            return list(line[:count])
        if depth == 16:
            return [line[2 * i] for i in range(count)]  # the high byte
        per = 8 // depth
        return [(line[i // per] >> (8 - depth * (i % per + 1))) & maxv for i in range(count)]

    def pixel(s):
        if ctype == 3:
            i = s[0]
            r, g, b = plte[3 * i:3 * i + 3] if 3 * i + 3 <= len(plte) else (0, 0, 0)
            a = trns[i] if trns and i < len(trns) else 255
            return r, g, b, a
        scale = 255 / maxv if depth < 8 else 1
        if ctype == 0:
            v = int(s[0] * scale)
            a = 0 if trns and struct.unpack('>H', trns[:2])[0] == s[0] else 255
            return v, v, v, a
        if ctype == 4:
            v = int(s[0] * scale)
            return v, v, v, int(s[1] * scale)
        r, g, b = (int(x * scale) for x in s[:3])
        if ctype == 2:
            a = 255
            if trns and len(trns) >= 6 and struct.unpack('>HHH', trns[:6]) == tuple(s[:3]):
                a = 0
            return r, g, b, a
        return r, g, b, int(s[3] * scale)

    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)] \
        if interlace else [(0, 0, 1, 1)]
    offset = 0
    for x0, y0, dx, dy in passes:
        pw = (w - x0 + dx - 1) // dx
        ph = (h - y0 + dy - 1) // dy
        if pw <= 0 or ph <= 0:
            continue
        rows, offset = unfilter(pw, ph, offset)
        for j, line in enumerate(rows):
            s = samples(line, pw * channels)
            for i in range(pw):
                out[y0 + j * dy][x0 + i * dx] = pixel(s[i * channels:(i + 1) * channels])
    return w, h, out


def _bmp(data):
    offset = struct.unpack('<I', data[10:14])[0]
    header = struct.unpack('<I', data[14:18])[0]
    w, h, _, bpp, comp = struct.unpack('<iiHHI', data[18:34])
    if comp not in (0, 3):
        raise ValueError('compressed BMP is not supported')
    top_down = h < 0
    h = abs(h)
    colors = struct.unpack('<I', data[46:50])[0] if header >= 40 else 0
    palette = []
    if bpp <= 8:
        n = colors or (1 << bpp)
        base = 14 + header
        palette = [(data[base + 4 * i + 2], data[base + 4 * i + 1], data[base + 4 * i]) for i in range(n)]
    masks = None
    if comp == 3:
        masks = struct.unpack('<III', data[54:66])
    elif bpp == 16:
        masks = (0x7C00, 0x03E0, 0x001F)
    stride = ((w * bpp + 31) // 32) * 4

    def channel(v, m):
        if not m:
            return 0
        shift = (m & -m).bit_length() - 1
        top = m >> shift
        return ((v & m) >> shift) * 255 // top

    out = []
    for row in range(h):
        y = row if top_down else h - 1 - row
        line = data[offset + y * stride:offset + (y + 1) * stride]
        px = []
        for x in range(w):
            if bpp <= 8:
                per = 8 // bpp
                i = (line[x // per] >> (8 - bpp * (x % per + 1))) & ((1 << bpp) - 1)
                px.append(palette[i] + (255,))
            elif bpp == 24:
                b, g, r = line[3 * x:3 * x + 3]
                px.append((r, g, b, 255))
            elif bpp == 16:
                v = line[2 * x] | line[2 * x + 1] << 8
                px.append((channel(v, masks[0]), channel(v, masks[1]), channel(v, masks[2]), 255))
            else:
                v = struct.unpack('<I', line[4 * x:4 * x + 4])[0]
                if masks:
                    px.append((channel(v, masks[0]), channel(v, masks[1]), channel(v, masks[2]), 255))
                else:
                    px.append((v >> 16 & 255, v >> 8 & 255, v & 255, 255))
        out.append(px)
    return w, h, out


def _pnm(data):
    tokens, pos = [], 2
    kind = data[:2]
    need = 2 if kind in (b'P1', b'P4') else 3
    while len(tokens) < need:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            while data[pos:pos + 1] not in (b'\n', b''):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        tokens.append(int(data[start:pos]))
    w, h = tokens[0], tokens[1]
    maxv = tokens[2] if need == 3 else 1
    pos += 1
    if kind in (b'P1', b'P2', b'P3'):
        values = [int(v) for v in data[pos:].split()]
    elif kind == b'P4':
        stride = (w + 7) // 8
        values = []
        for y in range(h):
            line = data[pos + y * stride:pos + (y + 1) * stride]
            values += [(line[x // 8] >> (7 - x % 8)) & 1 for x in range(w)]
    else:
        size = 2 if maxv > 255 else 1
        body = data[pos:]
        values = list(body) if size == 1 else [body[i] << 8 | body[i + 1] for i in range(0, len(body) - 1, 2)]
    out = []
    per = 3 if kind in (b'P3', b'P6') else 1
    for y in range(h):
        row = []
        for x in range(w):
            s = values[(y * w + x) * per:(y * w + x + 1) * per]
            if kind in (b'P1', b'P4'):
                v = 0 if s[0] else 255
                row.append((v, v, v, 255))
            elif per == 1:
                v = s[0] * 255 // maxv
                row.append((v, v, v, 255))
            else:
                row.append(tuple(c * 255 // maxv for c in s) + (255,))
        out.append(row)
    return w, h, out


def _gif(data):
    w, h, flags = struct.unpack('<HHB', data[6:11])
    pos = 13
    gct = []
    if flags & 0x80:
        n = 2 << (flags & 7)
        gct = [tuple(data[pos + 3 * i:pos + 3 * i + 3]) for i in range(n)]
        pos += 3 * n
    transparent = None
    out = [[(0, 0, 0, 0)] * w for _ in range(h)]
    while pos < len(data):
        block = data[pos]
        if block == 0x21:  # extension
            label = data[pos + 1]
            pos += 2
            if label == 0xF9 and data[pos] >= 4 and data[pos + 1] & 1:
                transparent = data[pos + 4]
            while data[pos]:
                pos += data[pos] + 1
            pos += 1
        elif block == 0x2C:  # the first image
            x0, y0, iw, ih, iflags = struct.unpack('<HHHHB', data[pos + 1:pos + 10])
            pos += 10
            table = gct
            if iflags & 0x80:
                n = 2 << (iflags & 7)
                table = [tuple(data[pos + 3 * i:pos + 3 * i + 3]) for i in range(n)]
                pos += 3 * n
            min_code = data[pos]
            pos += 1
            sub = bytearray()
            while data[pos]:
                sub += data[pos + 1:pos + 1 + data[pos]]
                pos += data[pos] + 1
            # LZW
            clear, end = 1 << min_code, (1 << min_code) + 1
            size = min_code + 1
            dictionary = [[i] for i in range(clear)] + [[], []]
            indices, prev, bitpos = [], None, 0
            total = len(sub) * 8
            while bitpos + size <= total:
                code = 0
                for b in range(size):
                    code |= ((sub[(bitpos + b) >> 3] >> ((bitpos + b) & 7)) & 1) << b
                bitpos += size
                if code == clear:
                    dictionary = [[i] for i in range(clear)] + [[], []]
                    size, prev = min_code + 1, None
                    continue
                if code == end:
                    break
                if code < len(dictionary):
                    entry = dictionary[code]
                    if prev is not None:
                        dictionary.append(prev + [entry[0]])
                elif prev is not None:
                    entry = prev + [prev[0]]
                    dictionary.append(entry)
                else:
                    break
                indices += entry
                prev = entry
                if len(dictionary) == (1 << size) and size < 12:
                    size += 1
            rows = list(range(ih))
            if iflags & 0x40:  # interlaced
                rows = list(range(0, ih, 8)) + list(range(4, ih, 8)) + list(range(2, ih, 4)) + list(range(1, ih, 2))
            for j, y in enumerate(rows):
                for x in range(iw):
                    k = j * iw + x
                    if k >= len(indices) or not (0 <= y0 + y < h and 0 <= x0 + x < w):
                        continue
                    i = indices[k]
                    if i != transparent and i < len(table):
                        out[y0 + y][x0 + x] = table[i] + (255,)
            break
        else:
            break
    return w, h, out


def load_picture(path):
    data = open(path, 'rb').read()
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        return _png(data)
    if data[:6] in (b'GIF87a', b'GIF89a'):
        return _gif(data)
    if data[:2] == b'BM':
        return _bmp(data)
    if data[:1] == b'P' and data[1:2] in b'123456':
        return _pnm(data)
    try:
        from PIL import Image  # other formats, when Pillow is there
    except ImportError:
        raise SystemExit('%s: not PNG/GIF/BMP/PNM, and Pillow is not installed for other formats' % path)
    im = Image.open(path).convert('RGBA')
    w, h = im.size
    flat = list(im.getdata())
    return w, h, [flat[y * w:(y + 1) * w] for y in range(h)]


# ---------------------------------------------------------------- the cells

def cut(picture, fill, width, scale_x, scale_y, shift_x, shift_y):
    """width x 240 cells: the cell (u, v) is the scaled picture area
    [shift_x + u, +1) x [shift_y + v, +1), averaged exactly over its pixels."""
    w, h, px = picture
    kx, ky = 1.0 / scale_x, 1.0 / scale_y  # picture pixels per cell
    cells = []
    for v in range(ROWS):
        ya = (shift_y + v) * ky
        yb = ya + ky
        row = []
        for u in range(width):
            xa = (shift_x + u) * kx
            xb = xa + kx
            acc = [0.0, 0.0, 0.0]
            area = 0.0
            for yy in range(math.floor(ya), math.ceil(yb)):
                fy = min(yb, yy + 1) - max(ya, yy)
                if fy <= 0:
                    continue
                for xx in range(math.floor(xa), math.ceil(xb)):
                    fx = min(xb, xx + 1) - max(xa, xx)
                    if fx <= 0:
                        continue
                    a = fx * fy
                    if 0 <= xx < w and 0 <= yy < h:
                        r, g, b, al = px[yy][xx]
                        t = al / 255.0
                        c = (r * t + fill[0] * (1 - t), g * t + fill[1] * (1 - t), b * t + fill[2] * (1 - t))
                    else:
                        c = fill
                    acc[0] += c[0] * a
                    acc[1] += c[1] * a
                    acc[2] += c[2] * a
                    area += a
            row.append((acc[0] / area, acc[1] / area, acc[2] / area))
        cells.append(row)
    return cells


def visible(width, scale):
    gx, gy, gw, gh = GAME[scale](width)
    return lambda u, v: not (gx <= u < gx + gw and gy <= v < gy + gh)


def palette_for(crops):
    """32 colours: k-means over the visible cells, a histogram of 5-bit colours
    keeps it quick; the start is the farthest-point choice."""
    hist = {}
    for width, scale, cells in crops:
        seen = visible(width, scale)
        for v, row in enumerate(cells):
            for u, c in enumerate(row):
                if seen(u, v):
                    key = (int(c[0]) >> 3, int(c[1]) >> 3, int(c[2]) >> 3)
                    e = hist.setdefault(key, [0.0, 0.0, 0.0, 0])
                    e[0] += c[0]
                    e[1] += c[1]
                    e[2] += c[2]
                    e[3] += 1
    points = [((e[0] / e[3], e[1] / e[3], e[2] / e[3]), e[3]) for e in hist.values()]
    if len(points) <= COLORS:
        pal = [p for p, _ in points]
    else:
        heaviest = max(points, key=lambda p: p[1])[0]
        pal = [heaviest]
        d2 = [sum((a - b) ** 2 for a, b in zip(p, heaviest)) for p, _ in points]
        while len(pal) < COLORS:
            i = max(range(len(points)), key=lambda j: d2[j] * points[j][1] ** 0.5)
            pal.append(points[i][0])
            d2 = [min(d2[j], sum((a - b) ** 2 for a, b in zip(points[j][0], points[i][0]))) for j in range(len(points))]
        for _ in range(20):
            sums = [[0.0, 0.0, 0.0, 0] for _ in pal]
            for p, n in points:
                j = min(range(COLORS), key=lambda k: (p[0] - pal[k][0]) ** 2 + (p[1] - pal[k][1]) ** 2 + (p[2] - pal[k][2]) ** 2)
                s = sums[j]
                s[0] += p[0] * n
                s[1] += p[1] * n
                s[2] += p[2] * n
                s[3] += n
            pal = [(s[0] / s[3], s[1] / s[3], s[2] / s[3]) if s[3] else pal[j] for j, s in enumerate(sums)]
    pal = [tuple(min(255, max(0, int(round(c)))) for c in p) for p in pal]
    pal += [(0, 0, 0)] * (COLORS - len(pal))
    return pal


def map_cells(cells, width, scale, pal, dither):
    seen = visible(width, scale)
    cache = {}

    def nearest(c):
        key = (int(c[0]), int(c[1]), int(c[2]))
        j = cache.get(key)
        if j is None:
            j = min(range(COLORS), key=lambda k: (c[0] - pal[k][0]) ** 2 + (c[1] - pal[k][1]) ** 2 + (c[2] - pal[k][2]) ** 2)
            cache[key] = j
        return j

    out = [[0] * width for _ in range(ROWS)]
    if not dither:
        for v in range(ROWS):
            for u in range(width):
                if seen(u, v):
                    out[v][u] = nearest(cells[v][u])
        return out
    err = [[[0.0, 0.0, 0.0] for _ in range(width)] for _ in range(ROWS)]
    for v in range(ROWS):
        forward = v % 2 == 0
        dx = 1 if forward else -1
        for u in (range(width) if forward else range(width - 1, -1, -1)):
            if not seen(u, v):
                continue
            c = [min(255.0, max(0.0, cells[v][u][i] + err[v][u][i])) for i in range(3)]
            j = nearest(c)
            out[v][u] = j
            e = [c[i] - pal[j][i] for i in range(3)]
            for ou, ov, f in ((dx, 0, 7 / 16), (-dx, 1, 3 / 16), (0, 1, 5 / 16), (dx, 1, 1 / 16)):
                uu, vv = u + ou, v + ov
                if 0 <= uu < width and vv < ROWS and seen(uu, vv):
                    for i in range(3):
                        err[vv][uu][i] += e[i] * f
    return out


def write_png(path, width, height, rgb_rows):
    raw = b''.join(b'\0' + bytes(row) for row in rgb_rows)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('picture')
    ap.add_argument('-o', '--output', help='the .gbbp file (default: the picture name with .gbbp)')
    ap.add_argument('--scale-x', type=float, default=1.0, help='cells per picture pixel across (default 1)')
    ap.add_argument('--scale-y', type=float, default=1.0, help='cells per picture pixel down (default 1)')
    ap.add_argument('--shift-x', type=float, default=0.0, help='TV-SOFT left edge in the scaled picture (default 0)')
    ap.add_argument('--shift-y', type=float, default=0.0, help='top edge in the scaled picture (default 0)')
    ap.add_argument('--scale-x3', type=float, help='3x: its own scale across (default: --scale-x)')
    ap.add_argument('--scale-y3', type=float, help='3x: its own scale down (default: --scale-y)')
    ap.add_argument('--shift-x3', type=float, help='3x: its own shift across (default: --shift-x)')
    ap.add_argument('--shift-y3', type=float, help='3x: its own shift down (default: --shift-y)')
    ap.add_argument('--dither', action='store_true', help='Floyd-Steinberg dithering (default: the nearest colour)')
    ap.add_argument('--fill', default='000000', help='RRGGBB outside the picture and under its transparency')
    ap.add_argument('--preview', help='a PNG with the four backplanes (the game window grey)')
    a = ap.parse_args()

    fill = tuple(int(a.fill[i:i + 2], 16) for i in (0, 2, 4))
    picture = load_picture(a.picture)
    print('%s: %dx%d' % (a.picture, picture[0], picture[1]))
    geometry = {
        2: (a.scale_x, a.scale_y, a.shift_x, a.shift_y),
        3: (a.scale_x3 or a.scale_x, a.scale_y3 or a.scale_y,
            a.shift_x if a.shift_x3 is None else a.shift_x3, a.shift_y if a.shift_y3 is None else a.shift_y3),
    }
    body = b'GBBP' + bytes([GBBP_VERSION])
    preview = []
    for width, extra in PARTS:
        crops = []
        for scale in (2, 3):
            sx, sy, dx, dy = geometry[scale]
            crops.append((width, scale, cut(picture, fill, width, sx, sy, dx + extra, dy)))
        pal = palette_for(crops)
        body += b''.join(bytes(p) for p in pal)
        for width_, scale, cells in crops:
            idx = map_cells(cells, width_, scale, pal, a.dither)
            body += b''.join(bytes(row) for row in idx)
            preview.append((width_, scale, idx, pal))
        print('%s: %d cells across, palette done' % ('TV-SOFT ' if width == 384 else 'HDMI/VGA', width))
    assert len(body) == 5 + sum(COLORS * 3 + 2 * w * ROWS for w, _ in PARTS)
    out = a.output or os.path.splitext(a.picture)[0] + '.gbbp'
    with open(out, 'wb') as f:
        f.write(body)
    print('%s: %d bytes' % (out, len(body)))

    if a.preview:
        # 2x2 pixels per cell, one under another: HDMI/VGA 2x, 3x, TV-SOFT 2x, 3x
        rows = []
        for width, scale, idx, pal in preview:
            seen = visible(width, scale)
            for v in range(ROWS):
                line = bytearray()
                for u in range(768 // 2):
                    c = pal[idx[v][u]] if u < width and seen(u, v) else (200, 200, 200) if u < width else (255, 255, 255)
                    line += bytes(c) * 2
                rows += [line, line]
        write_png(a.preview, 768, len(rows), rows)
        print('%s: preview' % a.preview)


if __name__ == '__main__':
    main()
