#!/usr/bin/env python3
"""Ocarina-Of-Time-X: the Xbox dashboard's two pictures, the title image (128x128) beside
the game and the save image (64x64) beside its save game.

Both are the game's own Ocarina of Time item icon (32x32 RGBA32), four and two
times over, pixel for pixel, on a dark ground, as Majora-X does with the Deku
Mask. Nothing of it is committed: the icon is read from data/oot.o2r (made from
your ROM), and the BMPs, the .rdf and the DXT1 .xbx the RXDK's bundler makes of
them land in build/gen/icons. tools/build.py runs this before imagebld, which
puts them in the XBE (/TITLEIMAGE, /DEFAULTSAVEIMAGE).

    python tools/make_icons.py
"""
import os
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVE = os.path.join(ROOT, "data", "oot.o2r")
OUT = os.path.join(ROOT, "build", "gen", "icons")
ICON = "textures/icon_item_static/gItemIconOcarinaOfTimeTex"
GROUND = (12, 16, 36)
RXDK = os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")
RDF = """// Dashboard %(what)s (%(size)dx%(size)d), from tools/make_icons.py: DXT1 is the only format the dashboard draws.

out_version        XPR0
out_packedresource %(name)s.xbx
out_header         %(name)s.h

Texture %(tex)s
{
   Source %(name)s.bmp
   Format D3DFMT_DXT1
   Levels 1
}
"""


def read_icon():
    """The icon's 32x32 pixels, [[(r, g, b, a)]], from its LUS texture resource (a 0x40-byte resource header, then
    type, width, height and size, then the N64's RGBA32 bytes)."""
    with zipfile.ZipFile(ARCHIVE) as z:
        d = z.read(ICON)
    kind, w, h, size = struct.unpack_from("<IIII", d, 0x40)
    if kind != 1 or size != w * h * 4 or len(d) < 0x50 + size:
        raise SystemExit("make_icons: %s is not an RGBA32 texture (type %d, %dx%d)" % (ICON, kind, w, h))
    px = d[0x50:0x50 + size]
    return w, h, [[tuple(px[(y * w + x) * 4:(y * w + x) * 4 + 4]) for x in range(w)] for y in range(h)]


def write_bmp(path, size, pixel):
    """A 24-bit BMP, `size` square, pixel(x, y) -> (r, g, b)."""
    row = size * 3
    pad = (4 - row % 4) % 4
    data = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b = pixel(x, y)
            data += bytes((b, g, r))
        data += bytes(pad)
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(hdr + info + data)
    os.replace(tmp, path)


def main():
    """The two .xbx paths (title image, save image), or None if there's no archive to take the icon from."""
    if not os.path.isfile(ARCHIVE):
        print("make_icons: no %s: the XBE goes without its dashboard pictures" % os.path.relpath(ARCHIVE, ROOT))
        return None
    w, h, rows = read_icon()
    os.makedirs(OUT, exist_ok=True)
    out = []
    for name, tex, what, size in (("titleimage", "TitleImage", "title image", 128),
                                  ("saveimage", "SaveImage", "save image", 64)):
        scale = size // w

        def pixel(x, y):
            r, g, b, a = rows[min(h - 1, y // scale)][min(w - 1, x // scale)]
            return tuple((c * a + gc * (255 - a)) // 255 for c, gc in zip((r, g, b), GROUND))

        write_bmp(os.path.join(OUT, name + ".bmp"), size, pixel)
        with open(os.path.join(OUT, name + ".rdf"), "w") as f:
            f.write(RDF % dict(name=name, tex=tex, what=what, size=size))
        r = subprocess.run([os.path.join(RXDK, "tools", "bundler.exe"), "-q", name + ".rdf"], cwd=OUT,
                           capture_output=True, text=True)
        xbx = os.path.join(OUT, name + ".xbx")
        if r.returncode != 0 or not os.path.isfile(xbx):
            sys.stdout.write(r.stdout + r.stderr)
            raise SystemExit("make_icons: bundler failed on %s.rdf" % name)
        out.append(xbx)
    print("make_icons: %s (%d bytes), %s (%d bytes)" % (
        os.path.relpath(out[0], ROOT), os.path.getsize(out[0]), os.path.relpath(out[1], ROOT),
        os.path.getsize(out[1])))
    return out


if __name__ == "__main__":
    main()
