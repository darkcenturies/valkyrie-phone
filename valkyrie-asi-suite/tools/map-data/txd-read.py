#!/usr/bin/env python3
"""
txd-read.py -- Turn the game's texture dictionaries into ordinary images.

analyse-textures.py already reads the headers, because counting what a texture
costs needs nothing more. Drawing one needs the pixels, and the game keeps those
in half a dozen different layouts depending on how old the texture is and which
tool last touched it.

A texture dictionary is sections again, same as a model:

    0x16 TextureDictionary
      0x15 TextureNative
        0x0001 Struct    platform, name, raster format, size, then the pixels

The pixels are one of:

    DXT1/3/5   blocks the graphics card understands. Wrapped in a DDS header and
               handed to Pillow, which decodes them far faster than we could.
    PAL8       a 256-colour palette followed by one byte per pixel. Most of San
               Andreas is this, because it was built for a 2004 console.
    8888/888   plain pixels, stored blue first.
    1555/565/4444  the same, squeezed into sixteen bits.

Only the largest mipmap is read. The smaller ones are the same picture again.
"""
import io
import os
import struct
import sys

TEXDICT, TEXNATIVE, STRUCT = 0x16, 0x15, 0x0001

# DXT2 and DXT4 are DXT3 and DXT5 with the colour already multiplied by the
# alpha. The blocks are laid out identically, so they decode the same way, and
# the tools that wrote them into San Andreas mostly meant the plain kind anyway.
FOURCC = {
    0x31545844: "DXT1",
    0x32545844: "DXT3", 0x33545844: "DXT3",
    0x34545844: "DXT5", 0x35545844: "DXT5",
}

PALETTE_8 = 0x2000
PALETTE_4 = 0x4000
FORMAT_MASK = 0x0F00

FORMAT_1555, FORMAT_565, FORMAT_4444 = 0x0100, 0x0200, 0x0300
FORMAT_LUM8, FORMAT_8888, FORMAT_888, FORMAT_555 = 0x0400, 0x0500, 0x0600, 0x0A00


def sections(data, start, end):
    at = start
    while at + 12 <= end:
        kind, size, _lib = struct.unpack_from("<III", data, at)
        body = at + 12
        if size <= 0 or body + size > end:
            return
        yield kind, body, size
        at = body + size


def dds(width, height, fourcc, payload):
    """Wrap raw block-compressed pixels so Pillow will open them."""
    head = bytearray(128)
    head[0:4] = b"DDS "
    struct.pack_into("<I", head, 4, 124)
    struct.pack_into("<I", head, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000)
    struct.pack_into("<I", head, 12, height)
    struct.pack_into("<I", head, 16, width)
    struct.pack_into("<I", head, 20, len(payload))
    struct.pack_into("<I", head, 76, 32)
    struct.pack_into("<I", head, 80, 0x4)
    head[84:88] = fourcc.encode()
    struct.pack_into("<I", head, 108, 0x1000)
    return bytes(head) + payload


def unpack16(raw, count, kind):
    """The sixteen-bit layouts, widened to eight bits a channel."""
    out = bytearray(count * 4)
    for i in range(count):
        v = raw[i * 2] | (raw[i * 2 + 1] << 8)
        if kind == FORMAT_565:
            r, g, b, a = (v >> 11) & 31, (v >> 5) & 63, v & 31, 255
            r, g, b = r * 255 // 31, g * 255 // 63, b * 255 // 31
        elif kind == FORMAT_4444:
            a, r, g, b = (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15
            r, g, b, a = r * 17, g * 17, b * 17, a * 17
        else:                                   # 1555 and 555
            a = 255 if (kind == FORMAT_555 or v & 0x8000) else 0
            r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
            r, g, b = r * 255 // 31, g * 255 // 31, b * 255 // 31
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return bytes(out)


def pixels(name, width, height, fmt, raster, palette, raw):
    """One texture as an RGBA image, or None if we cannot read this layout."""
    from PIL import Image

    count = width * height

    if fmt == "raw" and palette is None:
        # Some dictionaries name a format we do not know. The payload size gives
        # it away: block-compressed pixels come to eight or sixteen bytes per
        # four-by-four block and nothing else lands on those numbers.
        blocks = ((width + 3) // 4) * ((height + 3) // 4)
        if len(raw) == blocks * 8:
            fmt = "DXT1"
        elif len(raw) == blocks * 16:
            fmt = "DXT3" if (raster & FORMAT_MASK) == FORMAT_4444 else "DXT5"

    if fmt in ("DXT1", "DXT3", "DXT5"):
        return Image.open(io.BytesIO(dds(width, height, fmt, raw))).convert("RGBA")

    if palette is not None:
        # One byte per pixel into a table of colours stored blue first.
        out = bytearray(count * 4)
        for i in range(min(count, len(raw))):
            p = raw[i] * 4
            out[i * 4] = palette[p + 2]
            out[i * 4 + 1] = palette[p + 1]
            out[i * 4 + 2] = palette[p]
            out[i * 4 + 3] = palette[p + 3]
        return Image.frombytes("RGBA", (width, height), bytes(out))

    kind = raster & FORMAT_MASK

    if kind == FORMAT_8888:
        out = bytearray(raw[:count * 4])
        out[0::4], out[2::4] = out[2::4], out[0::4]      # stored blue first
        return Image.frombytes("RGBA", (width, height), bytes(out))

    if kind == FORMAT_888:
        out = bytearray(count * 4)
        for i in range(count):
            out[i * 4] = raw[i * 4 + 2]
            out[i * 4 + 1] = raw[i * 4 + 1]
            out[i * 4 + 2] = raw[i * 4]
            out[i * 4 + 3] = 255
        return Image.frombytes("RGBA", (width, height), bytes(out))

    if kind == FORMAT_LUM8:
        return Image.frombytes("L", (width, height), raw[:count]).convert("RGBA")

    if kind in (FORMAT_1555, FORMAT_565, FORMAT_4444, FORMAT_555):
        return Image.frombytes("RGBA", (width, height),
                               unpack16(raw, count, kind))

    return None


def read_native(data, body, end):
    """One texture out of a native section."""
    head = next(sections(data, body, end), None)
    if not head or head[0] != STRUCT:
        return None
    _kind, sbody, _size = head
    if sbody + 88 > end:
        return None

    platform, _filter = struct.unpack_from("<II", data, sbody)
    if platform not in (8, 9):
        return None

    name = data[sbody + 8:sbody + 40].split(b"\0")[0].decode("latin-1", "replace")
    raster, d3d = struct.unpack_from("<II", data, sbody + 72)
    width, height = struct.unpack_from("<HH", data, sbody + 80)
    depth, levels, _rtype, compression = struct.unpack_from("<BBBB", data, sbody + 84)

    if not width or not height or width > 8192 or height > 8192:
        return None

    if platform == 9:
        fmt = FOURCC.get(d3d, "raw")
    else:
        fmt = ("DXT%d" % compression) if compression in (1, 3, 5) else "raw"

    at = sbody + 88
    palette = None
    if raster & PALETTE_8:
        palette = data[at:at + 1024]
        at += 1024
    elif raster & PALETTE_4:
        palette = data[at:at + 128]
        at += 128

    size = struct.unpack_from("<I", data, at)[0]
    raw = data[at + 4:at + 4 + size]
    if len(raw) < size:
        return None

    return {"name": name, "width": width, "height": height, "format": fmt,
            "raster": raster, "palette": palette, "raw": raw,
            "depth": depth, "levels": levels}


def read_txd(data):
    """Every texture in one dictionary, headers only."""
    out = []
    head = next(sections(data, 0, len(data)), None)
    if not head or head[0] != TEXDICT:
        return out
    _kind, body, size = head
    end = min(body + size, len(data))

    for kind, nbody, nsize in sections(data, body, end):
        if kind != TEXNATIVE:
            continue
        try:
            tex = read_native(data, nbody, nbody + nsize)
        except (struct.error, IndexError):
            tex = None
        if tex:
            out.append(tex)
    return out


def to_image(tex):
    """A texture as RGBA, or None if the layout is one we do not read."""
    try:
        return pixels(tex["name"], tex["width"], tex["height"], tex["format"],
                      tex["raster"], tex["palette"], tex["raw"])
    except Exception:
        return None


def main():
    path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else None
    textures = read_txd(open(path, "rb").read())
    print("  %s: %d texture(s)" % (os.path.basename(path), len(textures)))

    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    for tex in textures:
        image = to_image(tex)
        state = "ok" if image else "UNREADABLE"
        print("    %-32s %4dx%-4d %-6s %s"
              % (tex["name"], tex["width"], tex["height"], tex["format"], state))
        if image and out_dir:
            image.save(os.path.join(out_dir, tex["name"] + ".png"))


if __name__ == "__main__":
    main()
