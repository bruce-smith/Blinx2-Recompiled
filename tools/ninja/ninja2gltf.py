"""Blinx 2 stage and model sections (Ninja chunk models) -> glTF 2.0 (.glb).

    py -3 tools/ninja/ninja2gltf.py <default.xbe> <section> <out.glb> [--mix r01s01.mix]

Blinx 2 keeps its models in XBE sections: M<region><stage> per stage (M11 is
stage 01-01), G1-G5, MDL* (characters, enemies, gimmicks). They hold Sega
Ninja structures with absolute addresses inside the section:

  NJS_OBJECT (52 bytes)  evalflags, model*, pos[3], ang[3] (float radians
                         in Blinx 2, not BAMS), scl[3], child*, sibling*
  NJS_CNK_MODEL          vlist*, plist*, center[3], radius
  vertex list            chunks: u8 type, u8 flags, u16 size; u16 index
                         offset, u16 count; then the vertices
  polygon list           bit, tiny (texture id), material and strip chunks
  NJS_TEXLIST            NJS_TEXNAME*, count; a name is char[32] + 2 u32

The tool finds every object node in the section, builds the object trees
and writes each tree as a node hierarchy: model meshes from the triangle
strips, UVs, material colour and alpha, and textures by name from the
stage's .mix bundle when given (--mix; XPR0 textures are decoded to PNG).

Needs numpy and Pillow. The output comes from your own copy of the game:
do not redistribute it.
"""

import argparse
import io
import json
import math
import os
import struct
import sys
from collections import OrderedDict

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))


def texture_index(assets):
    """texture name (lower case) -> source: a loose .dds or a .mix entry."""
    index = {}
    for dp, _dn, fn in os.walk(assets):
        for f in fn:
            p = os.path.join(dp, f)
            fl = f.lower()
            if fl.endswith(".dds"):
                index.setdefault(fl[:-4], ("dds", p, 0))
            elif fl.endswith(".tga"):                 # palettised (@ names)
                index.setdefault(fl[:-4], ("tga", p, 0))
            elif fl.endswith(".icb"):                 # its colour variants
                index.setdefault("palette:" + fl[:-4], ("icb", p, 0))
            elif fl.endswith(".mix"):
                with open(p, "rb") as fh:
                    head = fh.read(0x800)
                if head[:4] != b"MIXF":
                    continue
                cnt, = struct.unpack_from("<I", head, 0x1C)
                for i in range(min(cnt, (0x800 - 0x20) // 32)):
                    nm = head[0x20 + i * 32:0x40 + i * 32].split(b"\0")[0].decode("latin1").lower()
                    if nm:
                        index.setdefault(nm, ("mix", p, i))
    return index


def dds_header(w, h, fourcc, size):
    hdr = struct.pack("<4sIIIIIII44s", b"DDS ", 124, 0x81007, h, w, size, 0, 1, b"\0" * 44)
    hdr += struct.pack("<II4s20s", 32, 4, fourcc, b"\0" * 20)
    hdr += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    return hdr


def palette_source(index, name, colour):
    """The colour variant of a palettised texture: @name00.tga takes its
    palette from @name_pNN.icb (NN = colour, 1-based), if there is one."""
    base = name.lower().rstrip("0123456789")
    return index.get("palette:%s_p%02d" % (base, colour))


def texture_png(src, palette=None):
    """A .dds, a palettised .tga or an XPR0 texture inside a .mix (DXT1/3/5)
    -> PNG bytes. palette: an .icb source whose palette replaces the .tga's."""
    from PIL import Image
    kind, path, i = src
    data = open(path, "rb").read()
    if kind == "tga":
        # Colour-mapped TGA: 8-bit indices and a 256-entry palette (the NV2A
        # P8 textures of the custom characters). An .icb is an 8x8 TGA whose
        # palette is one of the editor's colour choices.
        img = Image.open(io.BytesIO(data))
        if palette:
            img.putpalette(Image.open(palette[1]).getpalette())
        buf = io.BytesIO()
        img.convert("RGBA").save(buf, "PNG")
        return buf.getvalue()
    if kind == "mix":
        x = 0x800
        _m, _total, hsize = struct.unpack_from("<3I", data, x)
        _c, off, _l, fmt, _s = struct.unpack_from("<5I", data, x + 12 + i * 20)
        w, h = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
        kindf = (fmt >> 8) & 0xFF
        fourcc, bs = {0x0C: (b"DXT1", 8), 0x0E: (b"DXT3", 16), 0x0F: (b"DXT5", 16)}.get(kindf, (None, 0))
        if not fourcc:
            return None
        size = max(1, w // 4) * max(1, h // 4) * bs
        raw = data[x + hsize + off:x + hsize + off + size]
        data = dds_header(w, h, fourcc, size) + raw
    try:
        img = Image.open(io.BytesIO(data)).convert("RGBA")
    except Exception:
        return None
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()

# vertex chunk type -> bytes per vertex (Ninja NJD_CV_*). Blinx 2's type 44
# (weighted) is xyz, a packed normal, then a u16 weight (/255) and a u16 index.
VERTEX_STRIDE = {32: 16, 33: 32, 34: 12, 35: 16, 36: 16, 37: 16, 38: 16, 39: 16,
                 40: 16, 41: 24, 42: 28, 43: 28, 44: 20, 45: 28, 46: 28, 47: 28,
                 48: 16, 49: 20, 50: 20}


def xbe_sections(xbe):
    """name -> (va, raw offset, raw size), read from the XBE headers."""
    base, = struct.unpack_from("<I", xbe, 0x104)
    nsec, secaddr = struct.unpack_from("<II", xbe, 0x11C)
    out = {}
    for i in range(nsec):
        o = secaddr - base + i * 56
        flags, va, vsize, raw, rsize, nameaddr = struct.unpack_from("<6I", xbe, o)
        n = nameaddr - base
        name = xbe[n:xbe.index(b"\0", n)].decode("latin1")
        out[name] = (va, raw, rsize)
    return out


class Section:
    def __init__(self, xbe, name):
        self.va, raw, size = xbe_sections(xbe)[name]
        self.data = xbe[raw:raw + size]
        self.size = size

    def ok(self, p):
        return p == 0 or self.va <= p < self.va + self.size

    def off(self, p):
        return p - self.va

    def u8(self, p):
        return self.data[self.off(p)]

    def unpack(self, fmt, p):
        return struct.unpack_from(fmt, self.data, self.off(p))


def find_objects(sec):
    objs = []
    d = sec.data
    for o in range(0, sec.size - 52, 4):
        ev, mdl = struct.unpack_from("<II", d, o)
        if ev > 0xFF or not sec.ok(mdl):
            continue
        ch, sib = struct.unpack_from("<II", d, o + 44)
        if not (sec.ok(ch) and sec.ok(sib)) or (mdl == 0 and ch == 0):
            continue
        pos = struct.unpack_from("<3f", d, o + 8)
        ang = struct.unpack_from("<3f", d, o + 20)
        scl = struct.unpack_from("<3f", d, o + 32)
        if not all(0.001 < abs(s) < 1000 for s in scl):
            continue
        if not all(a == 0 or 1e-6 < abs(a) < 100 for a in ang) or not all(abs(x) < 1e6 for x in pos):
            continue
        if mdl:
            vl, pl = sec.unpack("<II", mdl)
            if not (sec.ok(vl) and sec.ok(pl)):
                continue
            if vl and sec.u8(vl) not in VERTEX_STRIDE and sec.u8(vl) != 255:
                continue
        objs.append(sec.va + o)
    return objs


def read_object(sec, p):
    ev, mdl = sec.unpack("<II", p)
    pos = sec.unpack("<3f", p + 8)
    ang = sec.unpack("<3f", p + 20)
    scl = sec.unpack("<3f", p + 32)
    ch, sib = sec.unpack("<II", p + 44)
    return dict(addr=p, flags=ev, model=mdl, pos=pos, ang=ang, scl=scl, child=ch, sibling=sib)


def unpack_normal(w):
    """Xbox NORMPACKED3: x bits 0-10 and y bits 11-21 (signed 11-bit), z bits
    22-31 (signed 10-bit). Checked against face normals of stage 01-01:
    median |cos| 0.9999."""
    def sx(v, bits):
        v &= (1 << bits) - 1
        return v - (1 << bits) if v >> (bits - 1) else v
    n = np.array([sx(w, 11) / 1023.0, sx(w >> 11, 11) / 1023.0, sx(w >> 22, 10) / 511.0])
    ln = np.linalg.norm(n)
    return tuple(n / ln) if ln > 1e-6 else (0.0, 1.0, 0.0)


def read_vertices(sec, vl):
    """Vertex chunks -> {index: (xyz, normal)}."""
    verts = {}
    p = vl
    while True:
        t = sec.u8(p)
        if t == 255 or t not in VERTEX_STRIDE:
            break
        ioff, count = sec.unpack("<HH", p + 4)
        stride = VERTEX_STRIDE[t]
        q = p + 8
        for i in range(count):
            v = q + i * stride
            xyz = sec.unpack("<3f", v)
            if t in (44, 48, 49, 50):                     # packed normal
                nrm = unpack_normal(sec.unpack("<I", v + 12)[0])
            elif t in (33, 41, 42, 43, 45, 46, 47):       # float normal
                nrm = sec.unpack("<3f", v + (16 if t == 33 else 12))
            else:
                nrm = None
            if t == 44:                                   # weighted: own index
                _w, k = sec.unpack("<HH", v + 16)
                verts[ioff + k] = (xyz, nrm)
            else:
                verts[ioff + i] = (xyz, nrm)
        p = q + count * stride
    return verts


def read_polys(sec, pl):
    """Polygon chunks -> list of (material state, triangles[(idx, u, v)*3])."""
    mats = []
    state = dict(tex=None, diffuse=(1, 1, 1, 1), blend=0, flags=0)
    out = []
    p = pl
    while True:
        t = sec.u8(p)
        if t == 255:
            break
        if t == 0:
            p += 2
            continue
        if 1 <= t <= 7:                               # bit chunks
            if t == 1:
                state["blend"] = sec.u8(p + 1)
            p += 2
            continue
        if t in (8, 9):                               # tiny: texture id
            _t, fl, v = sec.unpack("<BBH", p)
            state["tex"] = v & 0x1FFF
            state["flags"] = fl
            p += 4
            continue
        _t, fl, size = sec.unpack("<BBH", p)
        body = p + 4
        if 16 <= t <= 31:                             # material
            q = body
            if t & 1:                                 # diffuse present (17,19,21,23)
                b, g, r, a = sec.unpack("<4B", q)
                state["diffuse"] = (r / 255, g / 255, b / 255, a / 255)
            state["blend"] = fl
            p = body + size * 2
            continue
        if 64 <= t <= 75:                             # strips
            hdr, = sec.unpack("<H", body)
            nstrip, uo = hdr & 0x3FFF, hdr >> 14
            q = body + 2
            uv = t in (65, 66, 68, 69, 71, 72)
            uvscale = 1023.0 if t in (66, 69, 72) else 255.0
            has_n = t in (67, 68, 69)
            has_c = t in (70, 71, 72)
            tris = []
            for _s in range(nstrip):
                n, = sec.unpack("<h", q)
                q += 2
                rev = n < 0
                n = abs(n)
                sv = []
                for k in range(n):
                    idx, = sec.unpack("<H", q)
                    q += 2
                    u = v = 0.0
                    if uv:
                        u, v = sec.unpack("<hh", q)
                        q += 4
                        u /= uvscale
                        v /= uvscale
                    if has_n:
                        q += 6
                    if has_c:
                        q += 4
                    if k >= 2:
                        q += 2 * uo
                    sv.append((idx, u, v))
                for k in range(n - 2):
                    a, b, c = sv[k], sv[k + 1], sv[k + 2]
                    if (k & 1) ^ rev:
                        a, b = b, a
                    tris.append((a, b, c))
            out.append((dict(state), tris))
            # The size field is too small for long strips (a 44-vertex strip
            # says 196 bytes and takes 268), so the strips themselves say where
            # the next chunk is. Padding after them is null chunks.
            p = q
            continue
        p = body + size * 2                            # volume / unknown: skip
    return out


def euler_quat(ang, zxy):
    """Angles in radians -> quaternion (x, y, z, w); rotation X, then Y, then
    Z (or Z, X, Y with NJD_EVAL_ZXY_ANG) applied to the vertex."""
    rx, ry, rz = ang

    def q_axis(axis, a):
        s = math.sin(a / 2)
        return np.array([axis[0] * s, axis[1] * s, axis[2] * s, math.cos(a / 2)])

    def mul(a, b):
        ax, ay, az, aw = a
        bx, by, bz, bw = b
        return np.array([aw * bx + ax * bw + ay * bz - az * by,
                         aw * by - ax * bz + ay * bw + az * bx,
                         aw * bz + ax * by - ay * bx + az * bw,
                         aw * bw - ax * bx - ay * by - az * bz])
    qx, qy, qz = q_axis((1, 0, 0), rx), q_axis((0, 1, 0), ry), q_axis((0, 0, 1), rz)
    q = mul(mul(qy, qx), qz) if zxy else mul(mul(qz, qy), qx)
    return (q / np.linalg.norm(q)).tolist()


def node_matrix(o):
    """4x4 local matrix of an NJS_OBJECT: T * R * S."""
    x, y, z, w = euler_quat(o["ang"], o["flags"] & 0x20)
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * np.array(o["scl"])
    m[:3, 3] = o["pos"]
    return m


def skinned_mesh(sec, root):
    """A weighted chunk model (a character) in bind pose.

    Nodes write their vertices, in their own space, into one vertex buffer
    shared by the whole tree: plain chunks set whole vertices, weighted
    chunks (type 44; flags & 3 = start, middle, end) add weight * position.
    Strips index that buffer. Walked in draw order (node, children,
    siblings). A strip set is drawn as soon as all its vertices are complete
    (written, weights summing to 1): the body's strips hang off a node above
    the bones that write and blend its vertices, and later nodes reuse
    buffer slots for their own parts. Returns the nodes [(object, parent
    index, world matrix)] and the triangles [(material state, [(pos, normal, u, v,
    {joint: weight})] * 3)]."""
    nodes, tris, deferred = [], [], []
    buf = {}

    def draw(st, tl):
        out = []
        for tri in tl:
            if all(c[0] in buf for c in tri):
                out.append([(buf[c[0]][0].copy(), buf[c[0]][1].copy(), c[1], c[2],
                             dict(buf[c[0]][2])) for c in tri])
        if out:
            tris.append((st, out))

    def walk(p, parent, pw, depth):
        while p and depth < 256:
            o = read_object(sec, p)
            w = pw @ node_matrix(o)
            me = len(nodes)
            nodes.append((o, parent, w))
            if o["model"]:
                vl, pl = sec.unpack("<II", o["model"])
                q = vl
                while vl:
                    t, fl, _sz, ioff, count = sec.unpack("<BBHHH", q)
                    if t == 255 or t not in VERTEX_STRIDE:
                        break
                    stride = VERTEX_STRIDE[t]
                    for i in range(count):
                        v = q + 8 + i * stride
                        xyz = np.append(sec.unpack("<3f", v), 1.0)
                        nrm = unpack_normal(sec.unpack("<I", v + 12)[0]) if t in (44, 48, 49, 50) else (0.0, 1.0, 0.0)
                        pos, n = (w @ xyz)[:3], w[:3, :3] @ np.array(nrm)
                        if t == 44:
                            wt, k = sec.unpack("<HH", v + 16)
                            wt /= 255.0
                            k += ioff
                            if fl & 3 == 0 or k not in buf:
                                buf[k] = [wt * pos, wt * n, {me: wt}]
                            else:
                                e = buf[k]
                                e[0] = e[0] + wt * pos
                                e[1] = e[1] + wt * n
                                e[2][me] = e[2].get(me, 0.0) + wt
                        else:
                            buf[ioff + i] = [pos, n, {me: 1.0}]
                    q += 8 + count * stride
                if pl:
                    deferred.extend(read_polys(sec, pl))
                for st, tl in list(deferred):
                    if all(c[0] in buf and sum(buf[c[0]][2].values()) > 0.99
                           for tri in tl for c in tri):
                        draw(st, tl)
                        deferred.remove((st, tl))
            if o["child"]:
                walk(o["child"], me, w, depth + 1)
            p = o["sibling"]

    walk(root, -1, np.eye(4), 0)
    for st, tl in deferred:                      # never complete: best effort
        draw(st, tl)
    return nodes, tris


# The stages' object placement lists (Blinx 2 NA retail .data). Per stage
# index (region - 1) * 5 + (stage - 1), a pointer to {count, records*}; a
# record starts u32 type, pos[3], scale, rot[3] (radians), param. 0x491C18
# is the main list (0x491D08 is a near-identical variant the game picks in
# some modes); 0x491DF8 is a second list the game also spawns; 0x4923C8 are
# the enemies (0x5C-byte records; then a patrol target).
PLACEMENT_TABLES = (("placements", 0x491C18, 40), ("placements_2", 0x491DF8, 40),
                    ("enemies", 0x4923C8, 0x5C))


def stage_placements(xbe, section):
    """section M<r><s> -> [(group, [(type, pos, scale, rot, param)])]."""
    import re
    m = re.fullmatch(r"M(\d+)(\d)", section)
    if not m:
        return []
    index = (int(m.group(1)) - 1) * 5 + int(m.group(2)) - 1
    va0, raw, size = xbe_sections(xbe)[".data"]

    def u(va, fmt):
        return struct.unpack_from(fmt, xbe, raw + va - va0)
    out = []
    for name, table, rsize in PLACEMENT_TABLES:
        h, = u(table + 4 * index, "<I")
        if not va0 <= h < va0 + size - 8:
            continue
        count, rec = u(h, "<II")
        if not (0 < count < 4096 and va0 <= rec and rec + rsize * count <= va0 + size):
            continue
        objs = []
        for k in range(count):
            t, x, y, z, sc, rx, ry, rz, prm = u(rec + rsize * k, "<I8f")
            objs.append((t, (x, y, z), sc, (rx, ry, rz), prm))
        out.append((name, objs))
    return out


# The character editor (Blinx 2 NA retail). A preset (edit_char# mdl_param_list_t{s,g}NNN.edt, 108 bytes) is copied into the character's
# data (sub_001F3E60): bytes 0-10 -> +0x3C..0x46, 11-19 -> +0x48..0x50,
# 23 -> +0x51, 20/21 -> +0x52/0x53, 24-37 -> +0x55..0x62, 38 -> +0x54,
# 39-41 -> +0x63..0x65 (the sliders). sub_00172880 then writes the chosen
# texture names into the model's texture list: per part, the design byte
# indexes a table of 32-byte names, the next byte is the colour (palette
# _Pnn, 0 = the texture's own). (part, data offset, name table, slot).
EDT_TO_DATA = dict([(k, 0x3C + k) for k in range(11)] + [(11 + k, 0x48 + k) for k in range(9)]
                   + [(23, 0x51), (20, 0x52), (21, 0x53), (38, 0x54)]
                   + [(24 + k, 0x55 + k) for k in range(14)] + [(39 + k, 0x63 + k) for k in range(3)])
EDITOR_PARTS = {
    "sweeper": [("kao", 0x3C, 0x44B598, 6), ("eye", 0x3E, 0x44B910, 2), ("jacket", 0x40, 0x44BA90, 5),
                ("glove", 0x43, 0x44C030, 4), ("leg", 0x45, 0x44C2D0, 7), ("boot", 0x48, 0x44C610, 1),
                ("emblem", 0x4A, 0x44C8B0, 0), ("glass", 0x4C, 0x44CB70, 3), ("metal", 0x4E, 0x44CE10, 8),
                ("armband", 0x51, 0x44D110, 9)],
    "tomtom": [("skin", 0x3C, 0x44D1F0, 8), ("hige", 0x3E, 0x44D4F0, 5), ("jacket", 0x40, 0x44D5B0, 6),
               ("glove", 0x43, 0x44DB90, 4), ("pants", 0x45, 0x44DE30, 7), ("boot", 0x48, 0x44E270, 1),
               ("emblem", 0x4A, 0x44EC50, 0), ("glass", 0x4C, 0x44E550, 3), ("cap", 0x4E, 0x44E870, 2),
               ("band", 0x51, 0x44EB70, 10), ("tatoo", 0x52, 0x44EDB0, 12), ("tatoo2", 0x53, 0x44EDB0, 13)],
}
COLOURED = {"kao", "eye", "jacket", "glove", "leg", "boot", "emblem", "glass", "metal",
            "skin", "hige", "pants", "cap"}


def editor_textures(xbe, edt, names):
    """A preset applied to a character's texture list -> (names, {name:
    colour}, sliders). The team follows the list (Tom-Tom names are tom_)."""
    va0, raw, size = xbe_sections(xbe)[".data"]
    data = {EDT_TO_DATA[k]: edt[k] for k in EDT_TO_DATA}
    team = "tomtom" if any("tom_" in n for n in names) else "sweeper"
    names, colours = list(names), {}
    for part, off, table, slot in EDITOR_PARTS[team]:
        q = raw + table + 32 * data.get(off, 0) - va0
        nm = xbe[q:q + 32].split(b"\0")[0].decode("latin1")
        if not nm or slot >= len(names):
            continue
        names[slot] = nm
        if part in COLOURED and data.get(off + 1):
            colours[nm.lower()] = data[off + 1]
    sliders = [struct.unpack("b", bytes([edt[k]]))[0] for k in list(range(24, 42))]
    return names, colours, sliders


def find_motions(sec, nodes):
    """NJS_MOTIONs in a section for a skeleton of `nodes` nodes: mdata*,
    u32 frames, u16 type (1 pos, 2 ang, 4 scl), u16 inp_fn (low nibble =
    elements per node; 0x40 = 16-bit angles), directly after their mdata
    (per node, one pointer per element, then u16 key counts)."""
    out = []
    d = sec.data
    for o in range(0, sec.size - 12, 4):
        md, nf, ty, inp = struct.unpack_from("<IIHH", d, o)
        if not (sec.va <= md < sec.va + o and 0 < nf < 100000 and ty in (1, 2, 3, 5, 6, 7)):
            continue
        nel = inp & 0xF
        if nel != bin(ty).count("1"):
            continue
        ent = (6 * nel + 3) & ~3
        if sec.va + o - md == ent * nodes:
            out.append(sec.va + o)
    return out


def read_motion(sec, hdr, nodes):
    """-> frames, per node {"pos": [(f, xyz)], "ang": [(f, xyz radians)]}."""
    md, nf, ty, inp = sec.unpack("<IIHH", hdr)
    kinds = [k for k, bit in (("pos", 1), ("ang", 2), ("scl", 4)) if ty & bit]
    nel = len(kinds)
    ent = (6 * nel + 3) & ~3
    out = []
    for k in range(nodes):
        e = md + ent * k
        ptrs = sec.unpack("<%dI" % nel, e)
        counts = sec.unpack("<%dH" % nel, e + 4 * nel)
        keys = {}
        for kind, p, c in zip(kinds, ptrs, counts):
            if not p or not c:
                continue
            if kind == "ang" and inp & 0x40:          # u16 frame, s16 x, y, z
                ks = [sec.unpack("<H3h", p + 8 * i) for i in range(c)]
                keys[kind] = [(f, tuple(a * 2 * math.pi / 65536 for a in v)) for f, *v in ks]
            elif kind == "ang":                       # u32 frame, i32 BAMS
                ks = [sec.unpack("<I3i", p + 16 * i) for i in range(c)]
                keys[kind] = [(f, tuple(a * 2 * math.pi / 65536 for a in v)) for f, *v in ks]
            else:
                ks = [sec.unpack("<I3f", p + 16 * i) for i in range(c)]
                keys[kind] = [(f, tuple(v)) for f, *v in ks]
        out.append(keys)
    return nf, out


def sample_keys(keys, frame, angles):
    """Linear interpolation of a key list at `frame` (angles: the short way)."""
    if frame <= keys[0][0]:
        return np.array(keys[0][1])
    for (f0, v0), (f1, v1) in zip(keys, keys[1:]):
        if frame <= f1:
            t = (frame - f0) / max(f1 - f0, 1)
            a, b = np.array(v0), np.array(v1)
            if angles:
                b = a + (b - a + math.pi) % (2 * math.pi) - math.pi
            return a + (b - a) * t
    return np.array(keys[-1][1])


class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.j = OrderedDict(asset={"version": "2.0", "generator": "blinx2-recomp ninja2gltf"},
                             scene=0, scenes=[{"nodes": []}], nodes=[], meshes=[],
                             materials=[], textures=[], images=[],
                             samplers=[{"magFilter": 9729, "minFilter": 9729,
                                        "wrapS": 10497, "wrapT": 10497}],
                             accessors=[], bufferViews=[], buffers=[])

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.bin += data
        self.j["bufferViews"].append(v)
        return len(self.j["bufferViews"]) - 1

    def accessor(self, arr, kind, target):
        arr = np.ascontiguousarray(arr)
        comp = {np.dtype(np.float32): 5126, np.dtype(np.uint16): 5123}.get(arr.dtype, 5125)
        a = {"bufferView": self.view(arr.tobytes(), target), "componentType": comp,
             "count": int(arr.shape[0]), "type": kind}
        if kind == "VEC3" and comp == 5126:
            a["min"] = arr.min(axis=0).tolist()
            a["max"] = arr.max(axis=0).tolist()
        self.j["accessors"].append(a)
        return len(self.j["accessors"]) - 1

    def image(self, png):
        self.j["images"].append({"bufferView": self.view(png), "mimeType": "image/png"})
        self.j["textures"].append({"sampler": 0, "source": len(self.j["images"]) - 1})
        return len(self.j["textures"]) - 1

    def write(self, path):
        self.j["buffers"] = [{"byteLength": len(self.bin)}]
        for k in ("materials", "textures", "images"):
            if not self.j[k]:
                del self.j[k]
        js = json.dumps(self.j, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        bn = bytes(self.bin) + b"\0" * (-len(self.bin) % 4)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bn)))
            f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<II", len(bn), 0x004E4942) + bn)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("xbe")
    ap.add_argument("section")
    ap.add_argument("out")
    ap.add_argument("--objects", help="comma-separated section offsets of the object "
                    "trees to export (default: the stage's areas, from its table)")
    ap.add_argument("--all-trees", action="store_true",
                    help="every object tree in the section (props sit at their origin)")
    ap.add_argument("--collision", action="store_true",
                    help="also the collision trees, as a separate hidden-by-name node")
    ap.add_argument("--grid", type=float, metavar="SPACING",
                    help="lay the top-level groups out in a grid, SPACING units apart "
                         "(for libraries of parts or props that all sit at the origin)")
    ap.add_argument("--placements", action="store_true",
                    help="the stage's object placements (type, position, rotation) "
                         "as marker nodes, from the game's placement lists")
    ap.add_argument("--skin", action="store_true",
                    help="export the --objects trees as skinned meshes (characters: "
                         "the tree is the skeleton, weighted chunks give the weights)")
    ap.add_argument("--motions", metavar="SECTIONS",
                    help="with --skin: the motions in these sections (e.g. MOTPL,MOTP0) "
                         "that fit the skeleton, as glTF animations")
    ap.add_argument("--fps", type=float, default=30.0,
                    help="motion frames per second (default 30)")
    ap.add_argument("--edt", metavar="FILE",
                    help="with --skin: dress the character from an editor preset "
                         "(edit_char/mdl_param_list_t{s,g}NNN.edt): designs and colours")
    ap.add_argument("--colour", type=int, metavar="N",
                    help="colour variant N (1-19) for palettised (@) textures")
    ap.add_argument("--assets", help="blipks-extracted archives (assets/extracted), "
                    "for textures by name")
    args = ap.parse_args(argv)
    if args.section.lower() == "stages":
        # Every stage section (M<region><stage>, G<n>) into the folder `out`.
        import re
        rest = (argv if argv is not None else sys.argv[1:])[3:]
        os.makedirs(args.out, exist_ok=True)
        names = [k for k in xbe_sections(open(args.xbe, "rb").read())
                 if re.fullmatch(r"M\d+|G\d+", k)]
        names.sort(key=lambda k: (k[0] != "M", int(k[1:-1] or 0) if k[0] == "M" else 0, int(k[-1]) if k[0] == "M" else int(k[1:])))
        for k in names:
            print(f"== {k}")
            main([args.xbe, k, os.path.join(args.out, k + ".glb")] + rest)
        return

    xbe = open(args.xbe, "rb").read()
    sec = Section(xbe, args.section)
    objs = find_objects(sec)
    by = {p: read_object(sec, p) for p in objs}
    refs = {o["child"] for o in by.values()} | {o["sibling"] for o in by.values()}
    roots = [p for p in objs if p not in refs]
    print(f"{args.section}: {len(objs)} objects, {len(roots)} trees")

    # The stage table (in .data): per area, collision tree, model tree, texture
    # list. Found as a reference to a model tree that is followed by a pointer
    # to a valid texture list.
    def texlist_names(p):
        if not p or not (sec.va <= p < sec.va + sec.size - 8):
            return None
        np_, cnt = sec.unpack("<II", p)
        if not (sec.va <= np_ < sec.va + sec.size) or not 0 < cnt < 2000:
            return None
        names = []
        for i in range(cnt):
            q = sec.off(np_) + i * 36
            raw = sec.data[q:q + 32].split(b"\0")[0]
            if not raw or any(c < 0x20 or c > 0x7E for c in raw):
                return None
            names.append(raw.decode("latin1"))
        return names

    areas = []                                    # (collision root, model root, names)
    for r in roots:
        pat = struct.pack("<I", r)
        i = xbe.find(pat)
        while i >= 0:
            if i % 4 == 0 and i + 8 <= len(xbe):
                names = texlist_names(struct.unpack_from("<I", xbe, i + 4)[0])
                if names:
                    coll = struct.unpack_from("<I", xbe, i - 4)[0] if i >= 4 else 0
                    areas.append((coll if coll in by else 0, r, names))
                    break
            i = xbe.find(pat, i + 1)
    print(f"{len(areas)} areas with texture lists")

    # Models the code sets up itself (characters, enemies, gimmicks): the code
    # names the tree and its texture list as immediates close together.
    code_tex = {}
    for r in roots:
        if any(m == r for _c, m, _n in areas):
            continue
        pat = struct.pack("<I", r)
        i = xbe.find(pat)
        while i >= 0 and r not in code_tex:
            for d in range(-48, 49):
                j = i + d
                if d == 0 or j < 0 or j + 4 > len(xbe):
                    continue
                names = texlist_names(struct.unpack_from("<I", xbe, j)[0])
                if names:
                    code_tex[r] = names
                    break
            i = xbe.find(pat, i + 1)

    texidx = texture_index(args.assets) if args.assets else {}
    texcache = {}

    part_colours = {}

    def tex_index(name):
        key = name.lower()
        if key.endswith("_xx") and key not in texidx:   # per-language name
            key = key[:-3] + "_us"
        if key not in texcache:
            col = part_colours.get(key, args.colour)
            pal = palette_source(texidx, key, col) if col else None
            png = texture_png(texidx[key], pal) if key in texidx else None
            texcache[key] = g.image(png) if png else None
        return texcache[key]

    g = Glb()
    matcache = {}
    root = {"name": args.section, "scale": [1.0, 1.0, -1.0], "children": []}
    g.j["nodes"].append(root)
    g.j["scenes"][0]["nodes"].append(0)
    ntri = 0
    bad = []

    def material(st, names, solid=False):
        tname = names[st["tex"]] if names and st["tex"] is not None and st["tex"] < len(names) else None
        mkey = (tname, st["diffuse"], st["blend"], solid)
        if mkey not in matcache:
            mat = {"name": tname or ("collision" if solid else "untextured"),
                   "pbrMetallicRoughness": {"baseColorFactor": list(st["diffuse"]),
                                            "metallicFactor": 0.0,
                                            "roughnessFactor": 1.0},
                   "doubleSided": True}
            if tname and not solid:
                ti = tex_index(tname)
                if ti is not None:
                    mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": ti}
                    mat["alphaMode"] = "MASK"
                    mat["alphaCutoff"] = 0.5
            if st["diffuse"][3] < 1.0:
                mat["alphaMode"] = "BLEND"
                mat.pop("alphaCutoff", None)
            g.j["materials"].append(mat)
            matcache[mkey] = len(g.j["materials"]) - 1
        return matcache[mkey]

    def emit_skinned(r, names):
        """One skinned mesh; the tree's nodes are its joints."""
        nonlocal ntri
        nodes, tris = skinned_mesh(sec, r)
        flip = np.diag([1.0, 1.0, -1.0, 1.0])     # the root node's scale
        first = len(g.j["nodes"])
        for k, (o, parent, _w) in enumerate(nodes):
            g.j["nodes"].append({"name": "bone%03d_%08X" % (k, o["addr"]),
                                 "translation": list(o["pos"]),
                                 "rotation": euler_quat(o["ang"], o["flags"] & 0x20),
                                 "scale": list(o["scl"])})
            kids = root["children"] if parent < 0 else                 g.j["nodes"][first + parent].setdefault("children", [])
            kids.append(first + k)
        ibm = np.array([np.linalg.inv(flip @ w).T for _o, _p, w in nodes], np.float32)
        skin = {"joints": list(range(first, first + len(nodes))), "skeleton": first,
                "inverseBindMatrices": g.accessor(ibm.reshape(-1, 16), "MAT4", None)}
        g.j.setdefault("skins", []).append(skin)
        for msec in (args.motions.split(",") if args.motions else []):
            ms = Section(xbe, msec)
            for hdr in find_motions(ms, len(nodes)):
                nf, tracks = read_motion(ms, hdr, len(nodes))
                frames = np.arange(nf, dtype=np.float32)
                times = g.accessor((frames / args.fps).reshape(-1, 1), "SCALAR", None)
                g.j["accessors"][times]["min"] = [0.0]
                g.j["accessors"][times]["max"] = [float(frames[-1] / args.fps)]
                anim = {"name": "%s_%06X" % (msec, hdr - ms.va), "channels": [], "samplers": []}
                for k, keys in enumerate(tracks):
                    o = nodes[k][0]
                    for kind, path in (("pos", "translation"), ("ang", "rotation"), ("scl", "scale")):
                        if kind not in keys:
                            continue
                        vals = [sample_keys(keys[kind], f, kind == "ang") for f in frames]
                        if kind == "ang":
                            vals = [euler_quat(v, o["flags"] & 0x20) for v in vals]
                            for i in range(1, len(vals)):     # keep one hemisphere
                                if np.dot(vals[i], vals[i - 1]) < 0:
                                    vals[i] = [-c for c in vals[i]]
                        arr = np.array(vals, np.float32)
                        out = g.accessor(arr, "VEC4" if kind == "ang" else "VEC3", None)
                        anim["samplers"].append({"input": times, "output": out,
                                                 "interpolation": "LINEAR"})
                        anim["channels"].append({"sampler": len(anim["samplers"]) - 1,
                                                 "target": {"node": first + k, "path": path}})
                if anim["channels"]:
                    g.j.setdefault("animations", []).append(anim)
            print(f"{msec}: {len([a for a in g.j.get('animations', []) if a['name'].startswith(msec)])} motions")
        prims = []
        for st, tl in tris:
            pos, nrm, uv, jo, we = [], [], [], [], []
            for tri in tl:
                for xyz, n, u, v, wts in tri:
                    pos.append((flip[:3, :3] @ xyz))
                    n = flip[:3, :3] @ n
                    nrm.append(n / (np.linalg.norm(n) or 1.0))
                    uv.append((u, v))
                    top = sorted(((j, x) for j, x in wts.items() if x > 0),
                                 key=lambda kv: -kv[1])[:4] or [(0, 1.0)]
                    tot = sum(x for _j, x in top) or 1.0
                    jo.append([j for j, _x in top] + [0] * (4 - len(top)))
                    we.append([x / tot for _j, x in top] + [0.0] * (4 - len(top)))
            attrs = {"POSITION": g.accessor(np.array(pos, np.float32), "VEC3", 34962),
                     "NORMAL": g.accessor(np.array(nrm, np.float32), "VEC3", 34962),
                     "TEXCOORD_0": g.accessor(np.array(uv, np.float32), "VEC2", 34962),
                     "JOINTS_0": g.accessor(np.array(jo, np.uint16), "VEC4", 34962),
                     "WEIGHTS_0": g.accessor(np.array(we, np.float32), "VEC4", 34962)}
            prims.append({"attributes": attrs, "material": material(st, names)})
            ntri += len(tl)
        g.j["meshes"].append({"name": "skin_%08X" % r, "primitives": prims})
        g.j["nodes"].append({"name": "mesh_%08X" % r, "mesh": len(g.j["meshes"]) - 1,
                             "skin": len(g.j["skins"]) - 1})
        g.j["scenes"][0]["nodes"].append(len(g.j["nodes"]) - 1)

    def emit(p, parent_children, names, depth=0, solid=False):
        nonlocal ntri
        while p and p in by and depth < 64:
            o = by[p]
            node = {"name": "obj_%08X" % p, "translation": list(o["pos"]),
                    "rotation": euler_quat(o["ang"], o["flags"] & 0x20),
                    "scale": list(o["scl"])}
            if o["model"]:
                vl, pl = sec.unpack("<II", o["model"])
                try:
                    verts = read_vertices(sec, vl) if vl else {}
                    polys = read_polys(sec, pl) if pl else []
                except (struct.error, IndexError):
                    # The walk ran off the section: a chunk this reader does not
                    # know, or not a model after all. Skip it.
                    bad.append(o["model"])
                    verts, polys = {}, []
                prims = []
                for st, tris in polys:
                    pts, nrms, uvs, idx = [], [], [], []
                    for tri in tris:
                        if not all(v[0] in verts for v in tri):
                            continue
                        for v in tri:
                            idx.append(len(pts))
                            xyz, nrm = verts[v[0]]
                            pts.append(xyz)
                            nrms.append(nrm)
                            uvs.append((v[1], v[2]))
                    if not idx:
                        continue
                    attrs = {"POSITION": g.accessor(np.array(pts, np.float32), "VEC3", 34962),
                             "TEXCOORD_0": g.accessor(np.array(uvs, np.float32), "VEC2", 34962)}
                    if all(nm is not None for nm in nrms):
                        attrs["NORMAL"] = g.accessor(np.array(nrms, np.float32), "VEC3", 34962)
                    prims.append({"attributes": attrs,
                        "indices": g.accessor(np.array(idx, np.uint32), "SCALAR", 34963),
                        "material": material(st, names, solid)})
                    ntri += len(idx) // 3
                if prims:
                    g.j["meshes"].append({"name": "mdl_%08X" % o["model"], "primitives": prims})
                    node["mesh"] = len(g.j["meshes"]) - 1
            g.j["nodes"].append(node)
            parent_children.append(len(g.j["nodes"]) - 1)
            if o["child"]:
                node["children"] = []
                emit(o["child"], node["children"], names, depth + 1, solid)
                if not node["children"]:
                    del node["children"]
            p = o["sibling"]

    def group(name, parent):
        nd = {"name": name, "children": []}
        g.j["nodes"].append(nd)
        parent.append(len(g.j["nodes"]) - 1)
        return nd

    if args.objects:
        sel = [sec.va + int(x, 0) for x in args.objects.split(",")]
        part_colours.clear()
        names_of = dict(code_tex)
        names_of.update({m: n for _c, m, n in areas})
        for r in sel:
            if args.skin:
                names = names_of.get(r)
                if args.edt and names:
                    names, cols, sliders = editor_textures(xbe, open(args.edt, "rb").read(), names)
                    part_colours.update(cols)
                    print("preset textures:", ", ".join(
                        n + ("/P%02d" % cols[n.lower()] if n.lower() in cols else "") for n in names))
                emit_skinned(r, names)
            else:
                emit(r, root["children"], names_of.get(r))
    else:
        for k, (coll, model, names) in enumerate(areas):
            grp = group("area%d" % k, root["children"])
            emit(model, grp["children"], names)
            if args.collision and coll:
                cg = group("area%d_collision" % k, root["children"])
                emit(coll, cg["children"], None, solid=True)
        if args.all_trees:
            used = {m for _c, m, _n in areas} | {c for c, _m, _n in areas}
            props = group("other_trees", root["children"])
            for r in roots:
                if r not in used:
                    emit(r, props["children"], code_tex.get(r))
    if args.placements:
        # The lists are in the game's world frame: the models' frame turned
        # 180 degrees about x (y down). In the exported frame (z flipped) that
        # is y negated: markers go at the scene root at (x, -y, z), their
        # rotations mirrored in y. Checked on stage 01-01: half the markers
        # are within 10 units of the stage's vertices (median 10).
        for gname, objs in stage_placements(xbe, args.section):
            grp = {"name": gname, "children": []}
            g.j["nodes"].append(grp)
            g.j["scenes"][0]["nodes"].append(len(g.j["nodes"]) - 1)
            for t, pos, sc, rot, prm in objs:
                g.j["nodes"].append({"name": "obj%02X_%g" % (t, prm),
                                     "translation": [pos[0], -pos[1], pos[2]],
                                     "rotation": euler_quat((-rot[0], rot[1], -rot[2]), False),
                                     "scale": [sc or 1.0] * 3,
                                     "extras": {"type": t, "param": prm}})
                grp["children"].append(len(g.j["nodes"]) - 1)
            print(f"{gname}: {len(objs)} objects")
    if args.grid:
        kids = root["children"]
        side = max(1, int(math.ceil(math.sqrt(len(kids)))))
        for k, ni in enumerate(kids):
            nd = g.j["nodes"][ni]
            t = nd.get("translation", [0.0, 0.0, 0.0])
            nd["translation"] = [t[0] + (k % side) * args.grid, t[1], t[2] + (k // side) * args.grid]
    if bad:
        print(f"skipped {len(bad)} models that could not be read: "
              + ", ".join("0x%X" % (m - sec.va) for m in bad[:10]))
    g.write(args.out)
    print(f"wrote {args.out}: {len(g.j['meshes'])} meshes, {ntri} triangles, "
          f"{len(g.j.get('materials', []))} materials, {len(g.j.get('images', []))} textures")


if __name__ == "__main__":
    main()
