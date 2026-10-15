"""Blinx 2 stage and model sections (Ninja chunk models) -> glTF 2.0 (.glb).

    py -3 tools/ninja/ninja2gltf.py <default.xbe> <section> <out.glb> [--mix r01s01.mix]

Blinx 2 keeps its models in XBE sections: M<region><stage> per stage (M11 is
stage 01-01), G1-G5, MDL* (characters, enemies, gimmicks). They hold Sega
Ninja structures with absolute addresses inside the section:

  NJS_OBJECT (52 bytes)  evalflags, model*, pos[3], ang[3] (BAMS, 65536 =
                         360 degrees), scl[3], child*, sibling*
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


def texture_png(src):
    """A .dds or an XPR0 texture inside a .mix (DXT1/3/5) -> PNG bytes."""
    from PIL import Image
    kind, path, i = src
    data = open(path, "rb").read()
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

# vertex chunk type -> bytes per vertex (Ninja NJD_CV_*)
VERTEX_STRIDE = {32: 16, 33: 32, 34: 12, 35: 16, 36: 16, 37: 16, 38: 16, 39: 16,
                 40: 16, 41: 24, 42: 28, 43: 28, 44: 28, 45: 28, 46: 28, 47: 28,
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
        ang = struct.unpack_from("<3i", d, o + 20)
        scl = struct.unpack_from("<3f", d, o + 32)
        if not all(0.001 < abs(s) < 1000 for s in scl):
            continue
        if not all(abs(a) < 0x40000 for a in ang) or not all(abs(x) < 1e6 for x in pos):
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
    ang = sec.unpack("<3i", p + 20)
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
            if t in (48, 49, 50):                         # packed normal
                nrm = unpack_normal(sec.unpack("<I", v + 12)[0])
            elif t in (33, 41, 42, 43, 44, 45, 46, 47):   # float normal
                nrm = sec.unpack("<3f", v + (16 if t == 33 else 12))
            else:
                nrm = None
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
            # says 196 bytes and takes 268); the strips themselves, padded to
            # 4 bytes, say where the next chunk is.
            p = body + ((q - body + 3) & ~3)
            continue
        p = body + size * 2                            # volume / unknown: skip
    return out


def euler_quat(ang, zxy):
    """Ninja BAMS angles -> quaternion (x, y, z, w); rotation X, then Y, then
    Z (or Z, X, Y with NJD_EVAL_ZXY_ANG) applied to the vertex."""
    rx, ry, rz = (a * 2 * math.pi / 65536.0 for a in ang)

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
        comp = 5126 if arr.dtype == np.float32 else 5125
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


def main():
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
    ap.add_argument("--assets", help="blipks-extracted archives (assets/extracted), "
                    "for textures by name")
    args = ap.parse_args()

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

    def tex_index(name):
        key = name.lower()
        if key not in texcache:
            png = texture_png(texidx[key]) if key in texidx else None
            texcache[key] = g.image(png) if png else None
        return texcache[key]

    g = Glb()
    matcache = {}
    root = {"name": args.section, "scale": [1.0, 1.0, -1.0], "children": []}
    g.j["nodes"].append(root)
    g.j["scenes"][0]["nodes"].append(0)
    ntri = 0

    def emit(p, parent_children, names, depth=0, solid=False):
        nonlocal ntri
        while p and p in by and depth < 64:
            o = by[p]
            node = {"name": "obj_%08X" % p, "translation": list(o["pos"]),
                    "rotation": euler_quat(o["ang"], o["flags"] & 0x20),
                    "scale": list(o["scl"])}
            if o["model"]:
                vl, pl = sec.unpack("<II", o["model"])
                verts = read_vertices(sec, vl) if vl else {}
                prims = []
                for st, tris in (read_polys(sec, pl) if pl else []):
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
                    attrs = {"POSITION": g.accessor(np.array(pts, np.float32), "VEC3", 34962),
                             "TEXCOORD_0": g.accessor(np.array(uvs, np.float32), "VEC2", 34962)}
                    if all(nm is not None for nm in nrms):
                        attrs["NORMAL"] = g.accessor(np.array(nrms, np.float32), "VEC3", 34962)
                    prims.append({"attributes": attrs,
                        "indices": g.accessor(np.array(idx, np.uint32), "SCALAR", 34963),
                        "material": matcache[mkey]})
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
        names_of = dict(code_tex)
        names_of.update({m: n for _c, m, n in areas})
        for r in sel:
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
    if args.grid:
        kids = root["children"]
        side = max(1, int(math.ceil(math.sqrt(len(kids)))))
        for k, ni in enumerate(kids):
            nd = g.j["nodes"][ni]
            t = nd.get("translation", [0.0, 0.0, 0.0])
            nd["translation"] = [t[0] + (k % side) * args.grid, t[1], t[2] + (k // side) * args.grid]
    g.write(args.out)
    print(f"wrote {args.out}: {len(g.j['meshes'])} meshes, {ntri} triangles, "
          f"{len(g.j.get('materials', []))} materials, {len(g.j.get('images', []))} textures")


if __name__ == "__main__":
    main()
