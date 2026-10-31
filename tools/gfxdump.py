#!/usr/bin/env python3
"""Lista y edita volcados de la HLE gráfica (gfxdump.bin, gfx.h: gfx_dump_header).

Sirve para aislar un dibujo que sale mal en el RSX: se edita el volcado en la
Mac y se repite con `tools/rpcs3-run.py --replay` o `salsa64-run --raster-dump`.

  tools/gfxdump.py list gfxdump.bin [--draw 3,7] [--vtx 7]
  tools/gfxdump.py edit gfxdump.bin out.bin --keep 3,7 --set 7:blend=0 --white 7
  tools/gfxdump.py tex gfxdump.bin 7 0 tex.ppm     (textura 0 del dibujo 7: color y, debajo, alfa)
  tools/gfxdump.py pixel gfxdump.bin 161 153      (triángulos que cubren ese píxel, en orden)

--keep deja solo esos dibujos (los demás quedan con 0 vértices; destinos y
borrados se conservan). --set N:campo=valor cambia un campo de un dibujo
(cycle, cull, depth, blend, atest, aref, fog, ntex, cvg_bit, tex0.flags...). --white N
pinta de blanco opaco la textura 0 del dibujo N. --k N:t,j=r,g,b,a cambia un
coeficiente de la combinación lineal; --normw N divide los vértices entre w.
"""
import argparse
import struct
import sys

MAGIC = 0x53344744
OPS = {0: "END", 1: "TARGET", 2: "ZCLEAR", 3: "DRAW"}
U8_FIELDS = ["cycle", "cull", "depth", "blend", "atest", "aref", "fog", "ntex"]
BLEND = ["OFF", "ALPHA", "ADD", "KEEP", "MUL", "ADD_MUL", "ALPHA_INV", "ADD_ONE"]
# struct gfx_draw (gfx.h): desplazamientos desde el inicio del dibujo.
OFF_NTERMS, OFF_FACTORS, OFF_VISCVG, OFF_AA = 48, 49, 77, 78
OFF_CVG_BIT = 46                      # uint8_t cvg_bit, detrás de prim_tile (16 bits)
CVG_BIT = ["ONE", "KEEP", "ALPHA", "MAX"]  # GFX_CVG_*: el bit de alfa en una imagen de 16 bits
OFF_TEX, TEX_SIZE = 80, 40            # struct gfx_tex tex[2]
OFF_K, K_TERMS = 160, 7               # float k[7][3][4]
TERM_NAMES = ["T0", "T0a", "T1", "T1a", "1", "t5", "t6"]
FACTOR_NAMES = {1: "T0", 2: "T0a", 3: "T1", 4: "T1a", 5: "C", 6: "Ca"}
VTX_SIZE = 32                          # struct gfx_vertex: x y z w s t, rgba, relleno


def detect_endian(data):
    for e in (">", "<"):
        if struct.unpack_from(e + "I", data, 0)[0] == MAGIC:
            return e
    sys.exit("no es un volcado de Salsa64")


class Dump:
    def __init__(self, data):
        self.e = detect_endian(data)
        _, cb, vb, tb = struct.unpack_from(self.e + "4I", data, 0)
        self.cmd = bytearray(data[16:16 + cb])
        self.vtx = bytearray(data[16 + cb:16 + cb + vb])
        self.tex = bytearray(data[16 + cb + vb:16 + cb + vb + tb])

    def save(self, path):
        hdr = struct.pack(self.e + "4I", MAGIC, len(self.cmd), len(self.vtx), len(self.tex))
        with open(path, "wb") as f:
            f.write(hdr + self.cmd + self.vtx + self.tex)

    def records(self):
        off = 0
        while off + 16 <= len(self.cmd):
            op, size = struct.unpack_from(self.e + "HH", self.cmd, off)
            yield off, op, size
            if op == 0 or size < 16:
                return
            off += size

    def draws(self):
        n = 0
        for off, op, _ in self.records():
            if op == 3:
                yield n, off
                n += 1

    def tex_at(self, off, i):
        base = off + OFF_TEX + TEX_SIZE * i
        o, w, h, pitch, flags, where, key = struct.unpack_from(self.e + "IHHHBBI", self.cmd, base)
        xf = struct.unpack_from(self.e + "4f", self.cmd, base + 16)
        src, spitch, ssiz, sfmt = struct.unpack_from(self.e + "IHBB", self.cmd, base + 32)
        return base, dict(offset=o, width=w, height=h, pitch=pitch, flags=flags, where=where, key=key, xf=xf,
                          src_addr=src, src_pitch=spitch, src_siz=ssiz, src_fmt=sfmt)

    def vertex(self, i):
        x, y, z, w, s, t = struct.unpack_from(self.e + "6f", self.vtx, i * VTX_SIZE)
        r, g, b, a = self.vtx[i * VTX_SIZE + 24:i * VTX_SIZE + 28]
        return x, y, z, w, s, t, (r, g, b, a)

    def describe(self, n, off):
        e = self.e
        first, count = struct.unpack_from(e + "II", self.cmd, off + 4)
        zimg = struct.unpack_from(e + "I", self.cmd, off + 16)[0]
        sc = struct.unpack_from(e + "4H", self.cmd, off + 20)
        f = dict(zip(U8_FIELDS, self.cmd[off + 28:off + 36]))
        fogc, kind = struct.unpack_from(e + "2I", self.cmd, off + 36)
        cvg = self.cmd[off + OFF_CVG_BIT]
        s = (f"#{n:3d} off {off:6d} v {first}+{count} kind {kind} cyc {f['cycle']} "
             f"cull {f['cull']} depth {f['depth']} blend {BLEND[f['blend']] if f['blend'] < 4 else f['blend']} "
             f"atest {f['atest']}/{f['aref']} cvg {CVG_BIT[cvg] if cvg < 4 else cvg} "
             f"fog {f['fog']} ntex {f['ntex']} z {zimg:x} "
             f"sc {[x / 4 for x in sc]}")
        for i in range(f["ntex"]):
            _, t = self.tex_at(off, i)
            s += (f"\n      tex{i} off {t['offset']} {t['width']}x{t['height']} pitch {t['pitch']} "
                  f"flags {t['flags']:#x} key {t['key']:08x} xf {[round(x, 5) for x in t['xf']]} "
                  f"de {t['src_addr']:06x}/{t['src_pitch']} siz {t['src_siz']} fmt {t['src_fmt']}")
        nterms = self.cmd[off + OFF_NTERMS]
        if self.cmd[off + OFF_VISCVG]:
            s += "\n      viscvg"
        if self.cmd[off + OFF_AA]:
            s += "\n      aa (cobertura de 8 muestras)"
        k = struct.unpack_from(e + f"{K_TERMS * 12}f", self.cmd, off + OFF_K)
        for t in range(K_TERMS if nterms else 5):
            row = k[t * 12:(t + 1) * 12]
            if nterms:
                if t >= nterms:
                    break
                fac = self.cmd[off + OFF_FACTORS + 4 * t:off + OFF_FACTORS + 4 * t + 4]
                name = "·".join(FACTOR_NAMES.get(x, str(x)) for x in fac if x) or "1"
            elif any(abs(x) > 1e-6 for x in row):
                name = TERM_NAMES[t]
            else:
                continue
            s += f"\n      {name:7s} k0 {fmt4(row[0:4])} kC {fmt4(row[4:8])} kCa {fmt4(row[8:12])}"
        return s


def fmt4(v):
    return "(" + " ".join(f"{x:.3f}" for x in v) + ")"


def cmd_list(args):
    d = Dump(open(args.dump, "rb").read())
    print(f"comandos {len(d.cmd)} B, vértices {len(d.vtx)} B ({len(d.vtx) // 32}), texturas {len(d.tex)} B")
    for off, op, size in d.records():
        if op == 3:
            continue
        a, b, c = struct.unpack_from(d.e + "3I", d.cmd, off + 4)
        print(f"     off {off:6d} {OPS.get(op, op)} a {a:#x} b {b:#x} c {c:#x}")
    for n, off in d.draws():
        if args.draw is None or n in args.draw:
            print(d.describe(n, off))
    if args.vtx is not None:
        _, off = list(d.draws())[args.vtx]
        first, count = struct.unpack_from(d.e + "II", d.cmd, off + 4)
        for i in range(first, first + count):
            x, y, z, w, s, t, (r, g, b, a) = d.vertex(i)
            print(f"      v{i}: x {x:9.2f} y {y:9.2f} z {z:9.2f} w {w:9.2f}  s {s:7.2f} t {t:7.2f}  "
                  f"rgba {r} {g} {b} {a}  ndc {x / w:6.3f} {y / w:6.3f} {z / w:6.4f}")


def ints(s):
    return {int(x) for x in s.split(",") if x}


def cmd_edit(args):
    d = Dump(open(args.dump, "rb").read())
    draws = dict(d.draws())
    if args.keep is not None:
        for n, off in draws.items():
            if n not in args.keep:
                struct.pack_into(d.e + "I", d.cmd, off + 8, 0)
    for spec in args.set:
        n, kv = spec.split(":", 1)
        key, val = kv.split("=", 1)
        off = draws[int(n)]
        if key in U8_FIELDS:
            d.cmd[off + 28 + U8_FIELDS.index(key)] = int(val, 0)
        elif key == "aa":
            d.cmd[off + OFF_AA] = int(val, 0)
        elif key == "cvg_bit":
            d.cmd[off + OFF_CVG_BIT] = int(val, 0)
        elif key.startswith("tex") and "." in key:
            i, field = key[3:].split(".", 1)
            base, _ = d.tex_at(off, int(i))
            if field != "flags":
                sys.exit(f"campo desconocido: {key}")
            d.cmd[base + 10] = int(val, 0)
        else:
            sys.exit(f"campo desconocido: {key}")
    for spec in args.k:
        n, rest = spec.split(":", 1)
        tj, vals = rest.split("=", 1)
        t, j = (int(x) for x in tj.split(","))
        v = [float(x) for x in vals.split(",")]
        struct.pack_into(d.e + "4f", d.cmd, draws[int(n)] + OFF_K + 48 * t + 16 * j, *v)
    for n in args.normw:
        off = draws[n]
        first, count = struct.unpack_from(d.e + "II", d.cmd, off + 4)
        for i in range(first, first + count):
            x, y, z, w = struct.unpack_from(d.e + "4f", d.vtx, i * VTX_SIZE)
            struct.pack_into(d.e + "4f", d.vtx, i * VTX_SIZE, x / w, y / w, z / w, 1.0)
    for n in args.white:
        _, t = d.tex_at(draws[n], 0)
        for y in range(t["height"]):
            row = t["offset"] + y * t["pitch"]
            d.tex[row:row + 4 * t["width"]] = b"\xff" * (4 * t["width"])
    d.save(args.out)


def cmd_tex(args):
    """Textura `i` del dibujo `n` en PPM: color arriba, alfa (gris) abajo."""
    d = Dump(open(args.dump, "rb").read())
    _, t = d.tex_at(dict(d.draws())[args.n], args.i)
    w, h = t["width"], t["height"]
    with open(args.out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, 2 * h))
        for alpha in (False, True):
            for y in range(h):
                row = t["offset"] + y * t["pitch"]
                for x in range(w):
                    r, g, b, a = d.tex[row + 4 * x:row + 4 * x + 4]
                    f.write(bytes((a, a, a)) if alpha else bytes((r, g, b)))
    print(f"{args.out}: {w}x{h}, {t}")


def cmd_pixel(args):
    """Triángulos (en el orden en que se dibujan) que cubren el centro del píxel."""
    d = Dump(open(args.dump, "rb").read())
    px, py = args.x + 0.5, args.y + 0.5

    def edge(a, b):
        return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])
    for n, off in d.draws():
        first, count = struct.unpack_from(d.e + "II", d.cmd, off + 4)
        for k in range(0, count - 2, 3):
            v = [d.vertex(first + k + j) for j in range(3)]
            if any(vv[3] <= 0 for vv in v):
                continue
            p = [(vv[0] / vv[3], vv[1] / vv[3]) for vv in v]
            e = (edge(p[1], p[2]), edge(p[2], p[0]), edge(p[0], p[1]))
            if all(x >= 0 for x in e) or all(x <= 0 for x in e):
                print(d.describe(n, off).split("\n")[0] + f"  triángulo {k // 3}")
                for vv in v:
                    x, y, z, w, s, t, c = vv
                    print(f"      x {x / w:8.2f} y {y / w:8.2f} z {z / w:.4f} w {w:9.2f} s {s:8.2f} t {t:8.2f} rgba {c}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("dump")
    p.add_argument("--draw", type=ints, help="solo estos dibujos")
    p.add_argument("--vtx", type=int, help="vértices de este dibujo")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("edit")
    p.add_argument("dump")
    p.add_argument("out")
    p.add_argument("--keep", type=ints)
    p.add_argument("--set", action="append", default=[])
    p.add_argument("--white", type=int, action="append", default=[])
    p.add_argument("--k", action="append", default=[], help="N:t,j=r,g,b,a: término t, coeficiente j")
    p.add_argument("--normw", type=int, action="append", default=[], help="vértices de N con w = 1")
    p.set_defaults(fn=cmd_edit)
    p = sub.add_parser("tex")
    p.add_argument("dump")
    p.add_argument("n", type=int)
    p.add_argument("i", type=int)
    p.add_argument("out")
    p.set_defaults(fn=cmd_tex)
    p = sub.add_parser("pixel")
    p.add_argument("dump")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)
    p.set_defaults(fn=cmd_pixel)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
