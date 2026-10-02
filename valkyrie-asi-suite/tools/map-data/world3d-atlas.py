#!/usr/bin/env python3
"""
world3d-atlas.py -- Put a tile's textures on one sheet.

A draw call is per material, and a material is per texture, so a tile using
fifty textures costs fifty draw calls no matter how little of it you can see.
Across the whole world that came to roughly fifty-three thousand a frame, which
is where the frame rate went - not memory, which was sitting at a seventh of
what the browser would allow.

Putting every texture a tile uses onto a single sheet means the tile can be one
mesh and one draw call. The catch is that the game tiles its textures - the
coordinates run far outside nought-to-one, and on a shared sheet that would
wander into the neighbouring picture. The viewer's shader wraps them back with
fract() before looking them up, and each cell is given a border of its own
wrapped edges so that filtering has somewhere to read from.

    build(keys, textures) -> (image, {key: (u, v)}, grid, cell size in the sheet)
"""
import os

# The game's textures were exported at 128 across. Anything larger is wasted on
# a map seen from above, and the sheet has to hold fifty of them.
CONTENT = 128

# Each cell is surrounded by its own opposite edges, so that a filter reading
# just outside the cell finds the continuation of the texture rather than the
# next one along. Without it every tiling surface shows a seam.
BORDER = 8

CELL = CONTENT + BORDER * 2

# Sheets larger than this are refused by some hardware.
MAX_SHEET = 4096


def safe_levels(border):
    """How many times the sheet can be halved before a cell's border runs out.

    Mipmapping and anisotropic filtering both read wider than a single pixel,
    and on a shared sheet what lies just outside a cell is a different texture.
    The border is what they are allowed to read into. Once it has been halved
    down to a single pixel there is nothing left to protect the cell, and
    neighbouring textures start bleeding in - which is why the fault grew worse
    the further away a surface was.
    """
    level = 0
    while border >= 2:
        border //= 2
        level += 1
    return level


def capacity():
    """How many textures fit on one sheet."""
    across = MAX_SHEET // CELL
    return across * across


_cells = {}
_ORDER = []
_KEEP = 4000


def wrapped(path, content=CONTENT, border=BORDER):
    """One cell: the texture, ringed by its own opposite edges.

    Kept between tiles. The same wall texture turns up on hundreds of them, and
    re-reading and re-tiling it each time is the difference between this taking
    minutes and taking most of a day.
    """
    import numpy
    from PIL import Image

    tag = (path, content, border)
    if tag in _cells:
        return _cells[tag]

    with Image.open(path) as source:
        image = source.convert("RGBA")
        if image.size != (content, content):
            image = image.resize((content, content), Image.LANCZOS)
        flat = numpy.asarray(image)

    # Three by three, then the middle cut back out with a border of whatever
    # was next to it - which for a texture that repeats is its own far edge.
    tiled = numpy.tile(flat, (3, 3, 1))
    lo, hi = content - border, content * 2 + border
    cell = tiled[lo:hi, lo:hi]

    _cells[tag] = cell
    _ORDER.append(tag)
    if len(_ORDER) > _KEEP:
        del _cells[_ORDER.pop(0)]
    return cell


# Why a material ended up on the plain white cell, counted across the run.
#
# Nothing recorded this before. A texture that is not in the manifest was
# filtered out of `keys` without a word, and one that is in the manifest but
# will not decode was swallowed by a bare `except: continue` - and both come
# out the same way on screen, as a white surface. Which of the two it is
# decides where the fix goes, so the two are counted separately and the names
# are kept.
unplaced = {"unlisted": {}, "undecodable": {}, "oversized": 0}


def report():
    """A summary of everything that fell back to the white cell."""
    lines = []
    for kind in ("unlisted", "undecodable"):
        names = unplaced[kind]
        if not names:
            continue
        lines.append("  %d texture(s) %s - e.g. %s"
                     % (len(names), kind,
                        ", ".join(sorted(names)[:6])))
        for name, why in sorted(names.items())[:3]:
            if why:
                lines.append("      %s: %s" % (name, why))
    if unplaced["oversized"]:
        lines.append("  %d tile(s) had more textures than any sheet size could"
                     " hold - every material on those is white"
                     % unplaced["oversized"])
    return "\n".join(lines)


def build(keys, textures, folder, content=CONTENT, border=BORDER):
    """A sheet holding every texture named in `keys`.

    Returns the image, where each key sits on it in texture coordinates, where
    the plain white cell sits, and how much of a cell is real content.
    """
    from PIL import Image

    for k in keys:
        if not textures.entry(k):
            unplaced["unlisted"].setdefault(str(k), "")
    keys = [k for k in keys if textures.entry(k)]
    if not keys:
        return None, {}, 0, 0.0

    cell_size = content + border * 2

    # Cell nought is plain white, for surfaces that have no texture at all and
    # are drawn in their material's colour alone. Cheaper than a second mesh.
    across = 1
    while across * across < len(keys) + 1:
        across += 1

    if across * cell_size > MAX_SHEET:
        # A few city tiles draw on more textures than will fit at full size.
        # Halving them is far better than losing the tile, and at this scale
        # the difference is not visible.
        if content > 32:
            return build(keys, textures, folder, content // 2, border)
        # Nothing placed at all, so every material in this tile falls back to
        # the white cell. Worth saying out loud rather than returning empty.
        unplaced["oversized"] += 1
        return None, {}, 0, 0.0

    import numpy

    size = across * cell_size
    sheet = numpy.zeros((size, size, 4), dtype=numpy.uint8)
    placed = {}

    sheet[0:cell_size, 0:cell_size] = 255            # the plain white cell
    blank = (border / size, border / size)

    for n, key in enumerate(sorted(keys), start=1):
        entry = textures.entry(key)
        try:
            cell = wrapped(os.path.join(folder, entry["f"]), content, border)
        except Exception as e:
            # The texture is listed and its file is there, but it will not
            # decode. The material lands on the white cell exactly as if it had
            # never existed, so say which one and why.
            unplaced["undecodable"].setdefault(
                str(key), "%s: %s" % (type(e).__name__, e))
            continue

        col, row = n % across, n // across
        y, x = row * cell_size, col * cell_size
        sheet[y:y + cell_size, x:x + cell_size] = cell

        # Where the *content* starts, past the border, in texture coordinates.
        placed[key] = ((x + border) / size, (y + border) / size)

    sheet = Image.fromarray(sheet, "RGBA")
    return sheet, placed, blank, content / size
