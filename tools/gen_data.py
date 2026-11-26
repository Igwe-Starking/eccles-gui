#!/usr/bin/env python3
"""
gen_data.py - generates the data tables inside src/eccles_gui.c

  python3 tools/gen_data.py            rewrite the generated blocks in src/eccles_gui.c
  python3 tools/gen_data.py --check    fail if they are stale (CI runs this)

Blocks are the text between

  //>>> generated: <name> (tools/gen_data.py)
  //<<< generated: <name>

  font   the built in 5x8 proportional font (95 ascii glyphs + degree, bullet, multiplication sign)
  sine   the quarter wave table for eccles_gui_sin()
  meshes cube, pyramid, cylinder, sphere, torus, card

Do not edit the blocks by hand. Plain python 3, no dependencies.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "eccles_gui.c")

# ---------------------------------------------------------------- font
# classic 5x7 ascii font, 5 column bytes per character, bit 0 = top row, bit 7 = the descender row
FONT_COLS = """
00 00 00 00 00|00 00 5F 00 00|00 07 00 07 00|14 7F 14 7F 14|24 2A 7F 2A 12|23 13 08 64 62|36 49 56 20 50|00 08 07 03 00
00 1C 22 41 00|00 41 22 1C 00|2A 1C 7F 1C 2A|08 08 3E 08 08|00 80 70 30 00|08 08 08 08 08|00 00 60 60 00|20 10 08 04 02
3E 51 49 45 3E|00 42 7F 40 00|72 49 49 49 46|21 41 49 4D 33|18 14 12 7F 10|27 45 45 45 39|3C 4A 49 49 31|41 21 11 09 07
36 49 49 49 36|46 49 49 29 1E|00 00 14 00 00|00 40 34 00 00|00 08 14 22 41|14 14 14 14 14|00 41 22 14 08|02 01 59 09 06
3E 41 5D 59 4E|7C 12 11 12 7C|7F 49 49 49 36|3E 41 41 41 22|7F 41 41 41 3E|7F 49 49 49 41|7F 09 09 09 01|3E 41 41 51 73
7F 08 08 08 7F|00 41 7F 41 00|20 40 41 3F 01|7F 08 14 22 41|7F 40 40 40 40|7F 02 1C 02 7F|7F 04 08 10 7F|3E 41 41 41 3E
7F 09 09 09 06|3E 41 51 21 5E|7F 09 19 29 46|26 49 49 49 32|03 01 7F 01 03|3F 40 40 40 3F|1F 20 40 20 1F|3F 40 38 40 3F
63 14 08 14 63|03 04 78 04 03|61 59 49 4D 43|00 7F 41 41 41|02 04 08 10 20|00 41 41 41 7F|04 02 01 02 04|40 40 40 40 40
00 03 07 08 00|20 54 54 78 40|7F 28 44 44 38|38 44 44 44 28|38 44 44 28 7F|38 54 54 54 18|00 08 7E 09 02|18 A4 A4 9C 78
7F 08 04 04 78|00 44 7D 40 00|20 40 40 3D 00|7F 10 28 44 00|00 41 7F 40 00|7C 04 78 04 78|7C 08 04 04 78|38 44 44 44 38
FC 18 24 24 18|18 24 24 18 FC|7C 08 04 04 08|48 54 54 54 24|04 04 3F 44 24|3C 40 40 20 7C|1C 20 40 20 1C|3C 40 30 40 3C
44 28 10 28 44|4C 90 90 90 7C|44 64 54 4C 44|00 08 36 41 00|00 00 77 00 00|00 41 36 08 00|02 01 02 04 02
"""

# extra glyphs drawn as ascii art (rows from the top), codepoint -> rows
EXTRA = [
    (0x00B0, ["XX.", "X.X", "XX.", "..."]),  # degree sign
    (0x00D7, ["...", "...", "X.X", ".X.", "X.X"]),  # multiplication sign
    (0x2022, ["...", "...", "...", ".X.", "XXX", ".X."]),  # bullet (drawn at the x-height)
]


def font_block():
    chars = [c for c in FONT_COLS.strip().replace("\n", "|").split("|") if c.strip()]
    assert len(chars) == 95, len(chars)
    glyphs = []  # (w, ox, rows[8])
    for idx, c in enumerate(chars):
        cols = [int(b, 16) for b in c.split()]
        assert len(cols) == 5
        used = [i for i in range(5) if cols[i]]
        if not used:
            glyphs.append((0, 0, [0] * 8, 3 if idx == 0 else 3))
            continue
        lo, hi = used[0], used[-1]
        rows = []
        for r in range(8):
            v = 0
            for k, ci in enumerate(range(lo, hi + 1)):
                if (cols[ci] >> r) & 1:
                    v |= 0x80 >> k
            rows.append(v)
        w = hi - lo + 1
        glyphs.append((w, 0, rows, w + 1))  #leading blank columns are trimmed, so the bearing is 0
    # the space: 3 px wide (idx 0 handled above)
    for cp, art in EXTRA:
        rows = []
        w = max(len(a) for a in art)
        for r in range(8):
            v = 0
            if r < len(art):
                for k, ch in enumerate(art[r]):
                    if ch == "X":
                        v |= 0x80 >> k
            rows.append(v)
        glyphs.append((w, 0, rows, w + 1))
    # fix: extra glyph ox 0
    out = []
    out.append("static const uint8_t g_font5x8_bitmap[] = {")
    for i, (w, ox, rows, adv) in enumerate(glyphs):
        out.append("    " + ", ".join("0x%02X" % r for r in rows) + ",")
    out.append("};")
    out.append("static const eccles_gui_glyph_t g_font5x8_glyphs[] = {")
    for i, (w, ox, rows, adv) in enumerate(glyphs):
        out.append("    {%d, %d, 8, %d, 0, %d}," % (i * 8, w, ox, adv))
    out.append("};")
    out.append("static const eccles_gui_range_t g_font5x8_ranges[] = {")
    out.append("    {0x20, 95, 0}, {0xB0, 1, 95}, {0xD7, 1, 96}, {0x2022, 1, 97}")
    out.append("};")
    out.append("const eccles_gui_font_t eccles_gui_font_5x8 = {g_font5x8_glyphs, g_font5x8_bitmap, g_font5x8_ranges, 4, 9, 1};")
    return "\n".join(out)


# ---------------------------------------------------------------- sine
def sine_block():
    vals = [int(round(32767 * math.sin(i * math.pi / 2 / 256))) for i in range(257)]
    out = ["static const int16_t g_sin_q[257] = {"]
    for i in range(0, 257, 12):
        out.append("    " + ", ".join(str(v) for v in vals[i:i + 12]) + ",")
    out.append("};")
    return "\n".join(out)


# ---------------------------------------------------------------- meshes
def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, k):
    return (a[0] * k, a[1] * k, a[2] * k)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    l = math.sqrt(dot(a, a))
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-12 else (0.0, 0.0, 0.0)


class Mesh:
    def __init__(self, name):
        self.name = name
        self.v = []
        self.f = []
        self.fn = []  # face normals (outward)
        self.vn = []  # vertex normals
        self.uv = []

    def vert(self, p, vn, uv=None):
        self.v.append(p)
        self.vn.append(vn)
        self.uv.append(uv)
        return len(self.v) - 1

    def face(self, a, b, c, outward):
        """the winding rule: cross(b-a, c-a) points outward"""
        pa, pb, pc = self.v[a], self.v[b], self.v[c]
        g = cross(sub(pb, pa), sub(pc, pa))
        if dot(g, outward) < 0:
            b, c = c, b
        pa, pb, pc = self.v[a], self.v[b], self.v[c]
        g = cross(sub(pb, pa), sub(pc, pa))
        assert dot(g, outward) > 0, (self.name, a, b, c)
        self.f.append((a, b, c))
        self.fn.append(norm(outward))


def q12(x):
    v = int(round(x * 4096))
    assert -32768 <= v <= 32767
    return v


def q7(x):
    return max(-127, min(127, int(round(x * 127))))


def emit_mesh(m, has_uv):
    n = m.name
    out = []

    def arr(ctype, ident, vals, per=12):
        out.append("static const %s %s[] = {" % (ctype, ident))
        for i in range(0, len(vals), per):
            out.append("    " + ", ".join(str(x) for x in vals[i:i + per]) + ",")
        out.append("};")

    arr("int16_t", "m_%s_v" % n, [q12(c) for p in m.v for c in p], 12)
    arr("uint16_t", "m_%s_f" % n, [i for f in m.f for i in f], 12)
    arr("int8_t", "m_%s_n" % n, [q7(c) for p in m.fn for c in p], 12)
    arr("int8_t", "m_%s_vn" % n, [q7(c) for p in m.vn for c in p], 12)
    if has_uv:
        arr("uint8_t", "m_%s_uv" % n, [c for u in m.uv for c in u], 16)
    out.append("const eccles_gui_mesh_t eccles_gui_mesh_%s = {m_%s_v, m_%s_f, m_%s_n, m_%s_vn, NULL, %s, %d, %d};" % (
        n, n, n, n, n, ("m_%s_uv" % n) if has_uv else "NULL", len(m.v), len(m.f)))
    return "\n".join(out)


def u8(x):
    return max(0, min(255, int(round(x))))


def mesh_cube():
    m = Mesh("cube")
    faces = [((1, 0, 0), (0, 1, 0)), ((-1, 0, 0), (0, 1, 0)), ((0, 1, 0), (0, 0, 1)),
             ((0, -1, 0), (0, 0, 1)), ((0, 0, 1), (0, 1, 0)), ((0, 0, -1), (0, 1, 0))]
    for n, up in faces:
        r = cross(n, up)  # seen from outside: right = n x up
        ids = []
        for sr, su in ((-1, 1), (1, 1), (1, -1), (-1, -1)):
            p = add(n, add(mul(r, sr), mul(up, su)))
            uv = (u8((dot(p, r) + 1) / 2 * 255), u8((1 - dot(p, up)) / 2 * 255))
            ids.append(m.vert(p, n, uv))
        m.face(ids[0], ids[1], ids[2], n)
        m.face(ids[0], ids[2], ids[3], n)
    return m


def mesh_pyramid():
    m = Mesh("pyramid")
    apex = (0.0, 1.0, 0.0)
    base = [(-1.0, -1.0, -1.0), (1.0, -1.0, -1.0), (1.0, -1.0, 1.0), (-1.0, -1.0, 1.0)]
    for i in range(4):
        a, b = base[i], base[(i + 1) % 4]
        n = norm(cross(sub(b, a), sub(apex, a)))
        mid = ((a[0] + b[0] + apex[0]) / 3, (a[1] + b[1] + apex[1]) / 3, (a[2] + b[2] + apex[2]) / 3)
        if dot(n, mid) < 0:
            n = mul(n, -1)
        ia = m.vert(a, n, (0, 255))
        ib = m.vert(b, n, (255, 255))
        ic = m.vert(apex, n, (128, 0))
        m.face(ia, ib, ic, n)
    n = (0.0, -1.0, 0.0)
    ids = [m.vert(p, n, (u8((p[0] + 1) / 2 * 255), u8((p[2] + 1) / 2 * 255))) for p in base]
    m.face(ids[0], ids[1], ids[2], n)
    m.face(ids[0], ids[2], ids[3], n)
    return m


def mesh_cylinder(N=16):
    m = Mesh("cylinder")
    top, bot = [], []
    for i in range(N + 1):
        t = 2 * math.pi * i / N
        c, s = math.cos(t), math.sin(t)
        n = (c, 0.0, s)
        u = u8(i / N * 255)
        top.append(m.vert((c, 1.0, s), n, (u, 0)))
        bot.append(m.vert((c, -1.0, s), n, (u, 255)))
    for i in range(N):
        t = 2 * math.pi * (i + 0.5) / N
        n = (math.cos(t), 0.0, math.sin(t))
        m.face(top[i], bot[i], top[i + 1], n)
        m.face(top[i + 1], bot[i], bot[i + 1], n)
    for y, n in ((1.0, (0.0, 1.0, 0.0)), (-1.0, (0.0, -1.0, 0.0))):
        c = m.vert((0.0, y, 0.0), n, (128, 128))
        ring = []
        for i in range(N):
            t = 2 * math.pi * i / N
            ring.append(m.vert((math.cos(t), y, math.sin(t)), n, (u8((math.cos(t) + 1) / 2 * 255), u8((math.sin(t) + 1) / 2 * 255))))
        for i in range(N):
            m.face(c, ring[i], ring[(i + 1) % N], n)
    return m


def mesh_sphere():
    m = Mesh("sphere")
    t = (1 + math.sqrt(5)) / 2
    pts = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
           (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    pts = [norm(p) for p in pts]
    tris = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
            (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
    cache = {}

    def mid(a, b):
        k = (min(a, b), max(a, b))
        if k not in cache:
            p = norm(add(pts[a], pts[b]))
            pts.append(p)
            cache[k] = len(pts) - 1
        return cache[k]

    new = []
    for a, b, c in tris:
        ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
        new += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
    for p in pts:
        m.vert(p, p, None)
    for a, b, c in new:
        centre = norm(add(add(pts[a], pts[b]), pts[c]))
        m.face(a, b, c, centre)
    assert len(m.v) == 42 and len(m.f) == 80
    return m


def mesh_torus(NU=12, NV=8, R=0.7, r=0.3):
    m = Mesh("torus")
    ids = {}
    for i in range(NU):
        u = 2 * math.pi * i / NU
        for j in range(NV):
            v = 2 * math.pi * j / NV
            c = (R + r * math.cos(v))
            p = (c * math.cos(u), r * math.sin(v), c * math.sin(u))
            n = (math.cos(v) * math.cos(u), math.sin(v), math.cos(v) * math.sin(u))
            ids[(i, j)] = m.vert(p, n, None)
    for i in range(NU):
        for j in range(NV):
            a, b = ids[(i, j)], ids[((i + 1) % NU, j)]
            c, d = ids[((i + 1) % NU, (j + 1) % NV)], ids[(i, (j + 1) % NV)]
            uc = 2 * math.pi * (i + 0.5) / NU
            vc = 2 * math.pi * (j + 0.5) / NV
            n = (math.cos(vc) * math.cos(uc), math.sin(vc), math.cos(vc) * math.sin(uc))
            m.face(a, b, c, n)
            m.face(a, c, d, n)
    return m


def mesh_card(N=4):
    m = Mesh("card")
    n = (0.0, 0.0, -1.0)
    ids = {}
    for j in range(N + 1):
        for i in range(N + 1):
            x = -1 + 2 * i / N
            y = 1 - 2 * j / N
            ids[(i, j)] = m.vert((x, y, 0.0), n, (u8(i / N * 255), u8(j / N * 255)))
    for j in range(N):
        for i in range(N):
            a, b, c, d = ids[(i, j)], ids[(i + 1, j)], ids[(i + 1, j + 1)], ids[(i, j + 1)]
            m.face(a, b, c, n)
            m.face(a, c, d, n)
    return m


def meshes_block():
    parts = []
    for fn, uv in ((mesh_cube, True), (mesh_pyramid, True), (mesh_cylinder, True), (mesh_sphere, False),
                   (mesh_torus, False), (mesh_card, True)):
        m = fn()
        assert len(m.v) <= 192 and len(m.f) <= 256, (m.name, len(m.v), len(m.f))
        parts.append(emit_mesh(m, uv))
    return "\n".join(parts)


BLOCKS = {"font": font_block, "sine": sine_block, "meshes": meshes_block}


def rewrite(text):
    for name, fn in BLOCKS.items():
        start = "//>>> generated: %s (tools/gen_data.py)" % name
        end = "//<<< generated: %s" % name
        a = text.find(start)
        b = text.find(end)
        if a < 0 or b < 0 or b < a:
            raise SystemExit("markers for block '%s' not found in src/eccles_gui.c" % name)
        a_end = a + len(start)
        text = text[:a_end] + "\n" + fn() + "\n" + text[b:]
    return text


def main():
    with open(SRC, "r", encoding="utf-8") as fh:
        old = fh.read()
    new = rewrite(old)
    if "--check" in sys.argv:
        if new != old:
            print("generated blocks in src/eccles_gui.c are stale: run python3 tools/gen_data.py")
            sys.exit(1)
        print("generated data up to date")
        return
    if new != old:
        with open(SRC, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(new)
        print("src/eccles_gui.c updated")
    else:
        print("src/eccles_gui.c already up to date")


if __name__ == "__main__":
    main()
