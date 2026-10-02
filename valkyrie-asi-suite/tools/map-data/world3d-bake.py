#!/usr/bin/env python3
"""
world3d-bake.py -- Turn the world into 3D tiles a browser can stream.

Reads the layout written by gen-world-scene, the index written by
world3d-index, and the images written by world3d-textures, then writes one glTF
file per square of the map. A viewer loads the squares near the camera and drops
the rest, which is the only way a hundred thousand buildings fit in a web page.

Within a tile everything is merged into as few meshes as possible, grouped by
texture. A thousand separate meshes costs a thousand draw calls and a browser
will not thank you for it.

    world3d-bake.py SCENE.json INDEX.json OUT_DIR [--tile 512]
        [--region x0,y0,x1,y1] [--textures DIR] [--atlas] [--update] [--force]
        [--double-sided] [--name-misses]

Adding to a map that is already built:

    --update              build only the tiles that are not there yet, and keep
                          the entries for every tile this run did not look at
    --update --force      rebuild the tiles in scope even if they exist, which
                          with --region is how one area is redone on its own

Each tile carries its own version, so a browser re-fetches the tiles that
changed and nothing else.

Distances are game units; a 512-unit tile is roughly two city blocks.
"""
import importlib.util
import re
import json
import math
import os
import io
import struct
import sys
import time

# Draw both faces of everything in this run.
#
# The world is normally single-sided, which is right for buildings: you never
# see the back of a wall, and drawing it would be work thrown away. An interior
# is the exception. Its shell is built to be looked at from inside, so from
# outside you see straight through it into the middle - which is what the
# stadium did once it was let onto the map.
DOUBLE_SIDED = False

# What a navigation map leaves out. Only applied with --navigation, because
# this script also bakes the tiles the website and the faction visualiser
# stream and those want the world as it is.
#
# Street furniture and vegetation, matched on model name. Dropped whole, every
# triangle, and that is the point: the radar pack used to drop the
# alpha-tested `masked` pass instead, which is not the same thing. A street
# light is a masked pole and an opaque head, a tree a masked canopy and an
# opaque trunk, a billboard a masked support and an opaque board, so dropping
# one pass left the other half of each hanging in the air. Here, where
# instances still exist one at a time, the whole object can go.
#
# 2,738 models match, covering 58,496 of the world's 128,623 placements.
PROPS = re.compile(
    r"lamp|light|traffic|signal|billboard|advert|veg_|tree|palm|bush|plant"
    r"|pole|pylon|telegraph|streetsign|sign_|bollard|postbox|parkmeter"
    r"|hydrant|bench|litter|bin_|fence|wire", re.IGNORECASE)

# Low-detail stand-ins. gen-world-scene drops the ones whose names begin with
# "lod", which misses another 209 models - oilderricklod01, quarry_lodbit05,
# pylon_lodbig1_ - that carry it in the middle or at the end. Each of those is
# a crude copy sitting in the same place as the real thing, so what they
# actually produce is z-fighting: two surfaces at the same depth, flickering
# against each other as the camera moves.
#
# The exclusions are the English words that happen to contain the letters.
# None of them appear in this world's model list; they are here so that a
# floodlight added later is not mistaken for a stand-in.
LOD = re.compile(r"lod", re.IGNORECASE)
NOT_LOD = re.compile(r"flood|blood|clod|explod|melod|lodge", re.IGNORECASE)

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "dffreader", os.path.join(_here, "dff-to-gltf.py"))
dffreader = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dffreader)

_aspec = importlib.util.spec_from_file_location(
    "atlas", os.path.join(_here, "world3d-atlas.py"))
atlas = importlib.util.module_from_spec(_aspec)
_aspec.loader.exec_module(atlas)

UNTEXTURED = "untextured"

MERGED = ("merged", (255, 255, 255, 255))


def tint(bucket, colour, count):
    """Paint the last `count` vertices of a bucket."""
    r, g, b = (min(255, max(0, int(to_linear(c) * 255 + 0.5))) for c in colour[:3])
    for _ in range(count):
        bucket["col"].extend((r, g, b, 255))


# Where the sun sits, in game coordinates, with Z up. Fixed forever: this is a
# map of a world that does not change, so the light falling on it never changes
# either.
SUN = (-0.35, 0.45, 0.82)
AMBIENT = 0.45


def bake_light(g, count):
    """Turn each vertex's normal into the brightness it should be drawn at.

    The result looks the same as lighting it in the browser would - it is the
    same sum, against the same sun - but a map of a world that never changes has
    no reason to work that out sixty times a second forever. Doing it once, here,
    also means the normals never have to be shipped: twelve bytes a vertex, and
    the largest thing in these files after the positions themselves.
    """
    nrm, col = g["nrm"], g["col"]

    for i in range(count):
        x, y, z = nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]
        length = (x * x + y * y + z * z) ** 0.5
        facing = ((x * SUN[0] + y * SUN[1] + z * SUN[2]) / length
                  if length > 1e-9 else 0.0)

        shade = AMBIENT + (1.0 - AMBIENT) * max(0.0, facing)
        source = g.get("prelit", [])
        prelit = source[i] if i < len(source) else None
        for c in range(3):
            at = i * 4 + c
            # Untextured surfaces can carry all their appearance in vertex
            # colours. They are already lit; do not replace or light them twice.
            light = to_linear(prelit[c]) if prelit is not None else shade
            col[at] = min(255, int(col[at] * light + 0.5))


def to_linear(value):
    """A colour byte as glTF wants it.

    The game stores what the eye should see; glTF stores light, and the two are
    not the same scale. Handing one over as the other makes every tinted surface
    washed out.
    """
    v = value / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


# How far a texture coordinate may sensibly run before it is not a coordinate
# any more. Reject corrupt values outside the supported range.
UV_LIMIT = 4096.0


def sane_uv_shift(value):
    """The whole-number part to subtract, ignoring nonsense."""
    if not math.isfinite(value) or abs(value) > UV_LIMIT:
        return 0.0
    return math.floor(value)


def sane_uv(value, shift):
    """A texture coordinate the shader can actually use.

    Two separate faults, one cause. Coordinates in the thousands are real but
    beyond what a 32-bit float can resolve to a single texel, so wrapping them
    in the shader breaks into fine stripes - the whole-number shift fixes those,
    because wrapping does not notice it but precision does.

    Coordinates of 10^33 are not real at all. Wrapping those returns whatever
    falls out of the arithmetic, which lands on an unrelated part of the texture
    sheet: that is a flag drawn across a road. Nothing sensible can be recovered
    from them, so they collapse to a single point of their own texture, which is
    at least the right texture.
    """
    if not math.isfinite(value) or abs(value) > UV_LIMIT:
        return 0.0
    return value - shift


def quat_to_matrix(q):
    """The game stores rotation as a quaternion. This is it as a 3x3.

    Conjugated on the way in. The game's quaternion turns the world onto the
    object; a renderer wants the one that turns the object into the world, which
    is the opposite. Without this, anything not square to the map ends up facing
    the wrong way - which is why some objects looked misplaced rather than all
    of them.
    """
    x, y, z, w = q
    x, y, z = -x, -y, -z
    return (
        (1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)),
        (2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)),
        (2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)),
    )


# The world is roughly sixty thousand units corner to corner. A model claiming
# anything near this is corrupt, and a few of them are: nni_land128 reports
# itself as 7.7e31 units wide.
SANE_LIMIT = 100000.0


def sane(mesh):
    """Whether a model's vertices are believable.

    One bad vertex is not a cosmetic problem. It stretches the mesh's bounding
    box across the world, which ruins the culling, and it destroys the
    quantisation for the entire tile it lands in - the packer measured the error
    on one such tile at 6.6e21 per cent.
    """
    for x, y, z in mesh["vertices"]:
        if not (-SANE_LIMIT < x < SANE_LIMIT and
                -SANE_LIMIT < y < SANE_LIMIT and
                -SANE_LIMIT < z < SANE_LIMIT):
            return False
    return True


class ModelCache:
    """Read each model once. The same wall appears hundreds of times."""

    def __init__(self, index, limit=400):
        self.index = index
        self.cache = {}
        self.limit = limit
        self.misses = set()
        self.sizes = {}
        self.boxes = {}
        self.tints = {}

    def get(self, name):
        key = name.lower()
        if key in self.cache:
            return self.cache[key]

        record = self.index.get(key)
        if record is None:
            self.misses.add(key)
            self.cache[key] = None
            return None

        try:
            with open(record["src"], "rb") as f:
                f.seek(record["off"])
                data = f.read(record["len"])
            meshes = [m for m in dffreader.read_dff(data) if sane(m)]
        except Exception:
            meshes = []

        if len(self.cache) > self.limit:
            self.cache.clear()
        self.cache[key] = meshes
        return meshes

    def bounds(self, name):
        """The box the model fits inside, worked out once and kept."""
        key = name.lower()
        if key in self.boxes:
            return self.boxes[key]

        lo = [1e9, 1e9, 1e9]
        hi = [-1e9, -1e9, -1e9]
        for mesh in self.get(name) or []:
            for v in mesh["vertices"]:
                for n in range(3):
                    if v[n] < lo[n]: lo[n] = v[n]
                    if v[n] > hi[n]: hi[n] = v[n]

        box = None if hi[0] < lo[0] else (lo, hi)
        self.boxes[key] = box
        return box

    def colour(self, name, textures):
        """The one colour that best stands for this model.

        Whichever material covers the most of it wins, which for a building is
        its walls rather than its door handles.
        """
        key = name.lower()
        if key in self.tints:
            return self.tints[key]

        best, best_faces = (200, 200, 200), -1
        for mesh in self.get(name) or []:
            for mat, faces in mesh["groups"].items():
                if len(faces) <= best_faces:
                    continue
                texture = mesh["textures"][mat] if mat < len(mesh["textures"]) else None
                tint = textures.average(textures.resolve(name, texture))
                if tint is None:
                    tint = list(mesh["colours"][mat][:3]) if mat < len(mesh["colours"]) else None
                if tint is None:
                    continue
                best, best_faces = tuple(tint), len(faces)

        self.tints[key] = best
        return best

    def footprint(self, name):
        """How wide the model is on the ground, in game units.

        The far view keeps only things big enough to make out from a distance.
        Most of the world's vertices are bins, lamp posts, fences and trees
        repeated thousands of times, and none of them are visible from a mile up.
        """
        key = name.lower()
        if key in self.sizes:
            return self.sizes[key]

        meshes = self.get(name) or []
        lo_x = lo_y = 1e9
        hi_x = hi_y = -1e9
        for mesh in meshes:
            for vx, vy, _vz in mesh["vertices"]:
                lo_x = min(lo_x, vx); hi_x = max(hi_x, vx)
                lo_y = min(lo_y, vy); hi_y = max(hi_y, vy)

        width = 0.0 if hi_x < lo_x else max(hi_x - lo_x, hi_y - lo_y)
        self.sizes[key] = width
        return width


class Textures:
    """Which image a model's texture name refers to.

    A texture name on its own is not enough to find a picture: the same name
    appears in many dictionaries with different contents. The model says which
    dictionary it uses, and a dictionary may fall back to a parent, so resolving
    one name means following that chain the way the game does.
    """

    def __init__(self, data, folder="."):
        data = data or {}
        self.folder = folder
        self.models = data.get("models", {})
        self.parents = data.get("parents", {})
        self.files = data.get("files", {})
        self.misses = set()
        self.unreadable = set()
        # Every dictionary a texture name appears in, for the pool fallback
        # below. Built once; the manifest is already keyed the right way round.
        self.by_name = {}
        for key in self.files:
            _dict, _, tex = key.rpartition("/")
            self.by_name.setdefault(tex, []).append(key)
        # How often each dictionary is the right answer. Used to break a tie
        # between dictionaries that all carry a texture of the same name.
        self.dict_use = {}
        # What each fallback rescued, for the run's report.
        self.rescued = {"unique": 0, "identical": 0, "commonest": 0}
        # The last of the three fallbacks is the only one that guesses, and
        # measured against the tiles that actually render white it rescued
        # nothing at all - those surfaces resolve their textures perfectly and
        # are white for a different reason entirely. So it is off unless asked
        # for. The two exact rescues above stay on: they cannot be wrong.
        self.pool_guess = "--pool-guess" in sys.argv

    def _remember(self, key):
        txd = key.rpartition("/")[0]
        self.dict_use[txd] = self.dict_use.get(txd, 0) + 1
        return key

    def resolve(self, model, texture):
        """The picture a model's texture name refers to.

        The strict path first, exactly as the game loads it: the model's own
        dictionary, then its txdp parents. That is the authority and it is
        never second-guessed.
        """
        if not texture or not self.files:
            return UNTEXTURED

        txd = self.models.get(model.lower())
        name = texture.lower()
        seen = 0
        while txd and seen < 8:
            key = "%s/%s" % (txd, name)
            if key in self.files:
                return self._remember(key)
            txd = self.parents.get(txd)
            seen += 1

        # The strict path has run out, and this is where a surface used to go
        # white. It should not, and the game is the reason why: at runtime
        # every loaded dictionary sits in one pool, so a model whose own
        # dictionary is missing the texture still finds it in a neighbour that
        # happens to be resident. Baking has no "resident" - it has the whole
        # world at once - so the nearest honest equivalent is to look the name
        # up across the manifest and take it when the answer is not in doubt.
        #
        # Measured over this world: of 25,652 distinct texture names, 56% live
        # in exactly one dictionary and a further 21% appear in several that
        # all hold the same picture. Those need no judgement at all. The rest
        # are the generic ground and wall names - ws_rooftarmac1 is in 296
        # dictionaries - and for those the commonest dictionary among the
        # candidates is a guess, so it is counted separately and can be turned
        # off. A wrong road texture is still a road texture; a white one is
        # not anything.
        candidates = self.by_name.get(name)
        if candidates:
            if len(candidates) == 1:
                self.rescued["unique"] += 1
                return self._remember(candidates[0])
            colours = {tuple(self.files[k].get("c", ())) for k in candidates}
            if len(colours) == 1:
                # Several dictionaries, one picture duplicated between them.
                self.rescued["identical"] += 1
                return self._remember(candidates[0])
            if self.pool_guess:
                best = max(candidates,
                           key=lambda k: self.dict_use.get(k.rpartition("/")[0], 0))
                self.rescued["commonest"] += 1
                return self._remember(best)

        self.misses.add(name)
        return UNTEXTURED

    def entry(self, key):
        return self.files.get(key)

    def average(self, key):
        """The texture's overall colour, for when we are not drawing it."""
        entry = self.files.get(key)
        if entry and "c" in entry:
            return entry["c"]
        return None


# The corners of a box, and the four that make up each of its six sides.
BOX_FACES = (
    ((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 0, -1)),
    ((0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 0, 1), (0, 0, 1)),
    ((0, 0, 0), (0, 0, 1), (1, 0, 1), (1, 0, 0), (0, -1, 0)),
    ((0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1), (0, 1, 0)),
    ((0, 0, 0), (0, 1, 0), (0, 1, 1), (0, 0, 1), (-1, 0, 0)),
    ((1, 0, 0), (1, 0, 1), (1, 1, 1), (1, 1, 0), (1, 0, 0)),
)


def bake_boxes(instances, cache, textures, min_size=0.0,
               max_size=1500.0, oversized=None, ground_height=0.0):
    """The far view: every object reduced to a coloured box.

    Nothing about a doorway or a drainpipe survives a mile of distance, and the
    world's full geometry is thirty-seven million vertices, which no browser
    will hold. A box is twenty-four. Seen from far enough up this is what the
    city looks like anyway - and it is what the game's own distant shapes were
    meant to be, before it turned out they were not simplified at all.
    """
    groups = {}
    ground = []
    if oversized is None:
        oversized = {}

    ground_height = 0.0
    if "--ground" in sys.argv:
        ground_height = float(sys.argv[sys.argv.index("--ground") + 1])

    for inst in instances:
        box = cache.bounds(inst["m"])
        if box is None:
            continue

        lo, hi = box
        width = max(hi[0] - lo[0], hi[1] - lo[1])
        if min_size and width < min_size:
            continue

        # A handful of models have a stray vertex miles from the rest of them,
        # or are the sky itself. Boxing those draws a slab across the whole map,
        # which is what the spikes to the horizon were.
        if width > max_size or (hi[2] - lo[2]) > max_size:
            oversized[inst["m"].lower()] = round(max(width, hi[2] - lo[2]))
            continue

        # Roads, pavements and ground are flat, so they cost little for the
        # area they cover and boxing them is what turns a city into a field of
        # blocks. Those keep their real shape; anything that stands up is boxed.
        if ground_height and (hi[2] - lo[2]) <= ground_height:
            ground.append(inst)
            continue

        colour = cache.colour(inst["m"], textures)

        # Everything in the tile goes into one mesh, with each vertex carrying
        # its own colour. Grouping by colour instead gave four hundred meshes
        # per tile and tens of thousands of draw calls a frame, which is what
        # made it crawl.
        bucket = groups.setdefault(MERGED, {"pos": [], "nrm": [], "uv": [],
                                            "idx": [], "col": []})

        m = quat_to_matrix(inst["r"])
        px, py, pz = inst["p"]

        def place(point):
            x, y, z = point
            return (m[0][0] * x + m[0][1] * y + m[0][2] * z + px,
                    m[1][0] * x + m[1][1] * y + m[1][2] * z + py,
                    m[2][0] * x + m[2][1] * y + m[2][2] * z + pz)

        for face in BOX_FACES:
            base = len(bucket["pos"]) // 3
            nx, ny, nz = face[4]
            wn = (m[0][0] * nx + m[0][1] * ny + m[0][2] * nz,
                  m[1][0] * nx + m[1][1] * ny + m[1][2] * nz,
                  m[2][0] * nx + m[2][1] * ny + m[2][2] * nz)

            for corner in face[:4]:
                point = tuple(lo[n] if corner[n] == 0 else hi[n] for n in range(3))
                bucket["pos"].extend(place(point))
                bucket["nrm"].extend(wn)
            tint(bucket, colour, 4)

            bucket["idx"].extend((base, base + 1, base + 2,
                                  base, base + 2, base + 3))

    if ground:
        bake_tile(ground, cache, textures, 0.0, True, groups)

    return groups


def bake_tile(instances, cache, textures, min_size=0.0, flat=False, groups=None):
    """One tile's geometry, in world coordinates, grouped by texture."""
    if groups is None:
        groups = {}

    for inst in instances:
        if min_size and cache.footprint(inst["m"]) < min_size:
            continue

        meshes = cache.get(inst["m"])
        if not meshes:
            continue

        m = quat_to_matrix(inst["r"])
        px, py, pz = inst["p"]

        for mesh in meshes:
            verts = mesh["vertices"]
            uvs = mesh["uvs"]
            normals = mesh["normals"]
            has_normals = bool(normals) and len(normals) >= len(verts)
            has_uvs = bool(uvs) and len(uvs) >= len(verts)
            n = len(verts)

            for mat, faces in mesh["groups"].items():
                texture = None
                if mat < len(mesh["textures"]):
                    texture = mesh["textures"][mat]
                colour = (255, 255, 255, 255)
                if mat < len(mesh["colours"]):
                    colour = mesh["colours"][mat]
                key = (textures.resolve(inst["m"], texture), colour)
                if flat:
                    key = MERGED

                bucket = groups.setdefault(
                    key, {"pos": [], "nrm": [], "uv": [], "idx": [], "col": [], "prelit": []})
                base = len(bucket["pos"]) // 3

                # Only the vertices this material's faces actually use. A model
                # split across several textures used to be copied whole into
                # every one of them, which is where most of a tile's size came
                # from.
                remap = {}
                for face in faces:
                    for v in face:
                        if v < n and v not in remap:
                            remap[v] = base + len(remap)

                if not remap:
                    continue

                # One whole-number offset for the whole group, so every vertex
                # keeps its relationship to the others.
                shift_u = shift_v = 0.0
                if has_uvs and not flat:
                    first = next(iter(remap))
                    shift_u = sane_uv_shift(uvs[first][0])
                    shift_v = sane_uv_shift(uvs[first][1])

                for v in sorted(remap, key=remap.get):
                    source_colours = mesh.get("prelit", [])
                    bucket.setdefault("prelit", []).append(
                        source_colours[v] if not texture and v < len(source_colours) else None)
                    vx, vy, vz = verts[v]
                    bucket["pos"].append(m[0][0] * vx + m[0][1] * vy + m[0][2] * vz + px)
                    bucket["pos"].append(m[1][0] * vx + m[1][1] * vy + m[1][2] * vz + py)
                    bucket["pos"].append(m[2][0] * vx + m[2][1] * vy + m[2][2] * vz + pz)

                    if has_normals:
                        nx, ny, nz = normals[v]
                        bucket["nrm"].append(m[0][0] * nx + m[0][1] * ny + m[0][2] * nz)
                        bucket["nrm"].append(m[1][0] * nx + m[1][1] * ny + m[1][2] * nz)
                        bucket["nrm"].append(m[2][0] * nx + m[2][1] * ny + m[2][2] * nz)
                    else:
                        # No normals in the model. Left empty and filled in from
                        # the triangles later, rather than faked here.
                        bucket["nrm"].extend((0.0, 0.0, 0.0))

                    if flat:
                        pass          # nothing is textured; UVs would be dead weight
                    elif has_uvs:
                        # Shifted towards zero by a whole number of repeats.
                        #
                        # The shader wraps these with fract(), and a whole-number
                        # shift leaves that untouched - but the game runs some
                        # coordinates into the thousands, where a 32-bit float
                        # can no longer resolve a single texel. That is what put
                        # fine stripes across roads, stone and terrain, and only
                        # those: they are what tiles hardest.
                        bucket["uv"].append(sane_uv(uvs[v][0], shift_u))
                        bucket["uv"].append(sane_uv(uvs[v][1], shift_v))
                    else:
                        bucket["uv"].extend((0.0, 0.0))

                if flat:
                    average = textures.average(key[0] if key is not MERGED else
                                               textures.resolve(inst["m"], texture))
                    shade = colour[:3]
                    if average:
                        shade = [average[n] * colour[n] // 255 for n in range(3)]
                    tint(bucket, shade, len(remap))

                for face in faces:
                    a, b, c = face
                    if a in remap and b in remap and c in remap:
                        bucket["idx"].append(remap[a])
                        bucket["idx"].append(remap[b])
                        bucket["idx"].append(remap[c])

    for g in groups.values():
        fill_missing_normals(g)

    return groups


def fill_missing_normals(g):
    """Work out normals for any vertex whose model did not supply one.

    Plenty of the game's models carry no normals at all - the game lights those
    differently - and a surface without one renders black. Rather than invent a
    flat value, this adds up the facing of every triangle a vertex belongs to,
    which is what gives a building corners you can see.
    """
    pos, nrm = g["pos"], g["nrm"]
    count = len(pos) // 3

    missing = [i for i in range(count)
               if nrm[i * 3] == 0.0 and nrm[i * 3 + 1] == 0.0 and nrm[i * 3 + 2] == 0.0]
    if not missing:
        return
    wanted = set(missing)

    idx = g["idx"]
    for t in range(0, len(idx), 3):
        a, b, c = idx[t], idx[t + 1], idx[t + 2]
        if a not in wanted and b not in wanted and c not in wanted:
            continue

        ax, ay, az = pos[a * 3], pos[a * 3 + 1], pos[a * 3 + 2]
        bx, by, bz = pos[b * 3], pos[b * 3 + 1], pos[b * 3 + 2]
        cx, cy, cz = pos[c * 3], pos[c * 3 + 1], pos[c * 3 + 2]

        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        fx = uy * vz - uz * vy
        fy = uz * vx - ux * vz
        fz = ux * vy - uy * vx

        for v in (a, b, c):
            if v in wanted:
                nrm[v * 3] += fx
                nrm[v * 3 + 1] += fy
                nrm[v * 3 + 2] += fz

    for i in missing:
        x, y, z = nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]
        length = (x * x + y * y + z * z) ** 0.5
        if length > 1e-9:
            nrm[i * 3] = x / length
            nrm[i * 3 + 1] = y / length
            nrm[i * 3 + 2] = z / length
        else:
            nrm[i * 3 + 2] = 1.0   # degenerate face; point it up rather than nowhere


def merge_onto_sheet(groups, textures, content=None, border=None):
    """Everything in a tile as one or two meshes, sharing one texture sheet.

    Fifty separate materials is fifty draw calls, and that - not memory - is
    what held the frame rate down. Once every texture is on one sheet the whole
    tile can be a single mesh.

    Two, in fact. Anything with holes in it - fences, foliage, railings - has to
    be drawn with alpha testing and from both sides, and that cannot be mixed
    into the same draw as solid walls without one or the other looking wrong.

    Each vertex carries where its cell begins on the sheet, in the second set of
    texture coordinates. The shader wraps the original coordinates back into
    nought-to-one before offsetting into the cell, which is what keeps a road
    texture repeating instead of running into its neighbour.
    """
    keys = {key for key, _colour in groups}
    sheet, placed, blank, scale = atlas.build(
        keys, textures, textures.folder,
        content or atlas.CONTENT, border or atlas.BORDER)
    if sheet is None:
        return None, None, None, 0.0

    def empty():
        return {"pos": [], "uv": [], "uv2": [], "col": [], "idx": []}

    solid, masked = empty(), empty()

    for (key, colour), g in sorted(groups.items(), key=lambda kv: str(kv[0])):
        if not g["idx"]:
            continue

        count = len(g["pos"]) // 3

        # The material's own colour has nowhere else to live once every group
        # shares one material, so it goes onto the vertices - and the baked
        # sunlight is applied on top of it, exactly as before.
        if not g.get("col"):
            g["col"] = []
            r, gr, b = (min(255, max(0, int(to_linear(c) * 255 + 0.5)))
                        for c in colour[:3])
            for _ in range(count):
                g["col"].extend((r, gr, b, 255))
        bake_light(g, count)

        entry = textures.entry(key)
        cell = placed.get(key, blank)
        target = masked if (entry and entry["a"]) else solid

        base = len(target["pos"]) // 3
        target["pos"].extend(g["pos"])
        target["col"].extend(g["col"])

        if g["uv"] and len(g["uv"]) >= count * 2:
            target["uv"].extend(g["uv"][:count * 2])
        else:
            target["uv"].extend([0.0] * (count * 2))

        for _ in range(count):
            target["uv2"].extend(cell)

        for i in g["idx"]:
            target["idx"].append(base + i)

    return sheet, solid, masked, scale


def write_glb(groups, path, textures, flat=False):
    """One binary glTF holding every mesh in the tile."""
    blob = bytearray()
    views, accessors, meshes, materials, nodes = [], [], [], [], []
    images, samplers, gl_textures = [], [], []
    image_ids = {}

    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        blob.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            # Image bytes are not a vertex or index buffer and must not claim
            # to be one.
            view["target"] = target
        views.append(view)
        return len(views) - 1

    def add_texture(key):
        """A glTF texture for one of our images, made once per tile.

        The picture is copied into the tile rather than linked beside it. Linked
        images meant a tile asked the browser for several hundred separate files
        and a handful of tiles was enough to exhaust it outright. A texture used
        by two neighbouring tiles is therefore stored twice, which is the price
        of a tile being one download.
        """
        entry = textures.entry(key)
        if entry is None:
            return None, False
        if key in image_ids:
            return image_ids[key], entry["a"] == 1

        try:
            with open(os.path.join(textures.folder, entry["f"]), "rb") as f:
                blob_bytes = f.read()
        except OSError:
            # The manifest lists it but the image is not beside it. Worth
            # saying so: silently dropping it produced a whole world with no
            # textures at all and nothing in the output to suggest why.
            textures.unreadable.add(entry["f"])
            return None, False

        if not samplers:
            # The game's surfaces tile; anything else leaves seams down roads.
            samplers.append({"wrapS": 10497, "wrapT": 10497,
                             "magFilter": 9729, "minFilter": 9987})

        mime = "image/png" if entry["f"].endswith(".png") else "image/jpeg"
        images.append({"bufferView": add_view(blob_bytes, None), "mimeType": mime})
        gl_textures.append({"source": len(images) - 1, "sampler": 0})
        image_ids[key] = len(gl_textures) - 1
        return len(gl_textures) - 1, entry["a"] == 1

    for (key, colour), g in groups.items():
        if not g["idx"]:
            continue

        count = len(g["pos"]) // 3

        pos = struct.pack("<%df" % len(g["pos"]), *g["pos"])
        uv = struct.pack("<%df" % len(g["uv"]), *g["uv"]) if g["uv"] else b""
        if not g.get("col"):
            g["col"] = [255] * (count * 4)
        bake_light(g, count)
        col = bytes(g["col"])

        # Sixteen-bit indices wherever they fit, which is most of the time and
        # halves what the index buffer costs.
        if count <= 65535:
            idx = struct.pack("<%dH" % len(g["idx"]), *g["idx"])
            idx_type = 5123
        else:
            idx = struct.pack("<%dI" % len(g["idx"]), *g["idx"])
            idx_type = 5125

        xs = g["pos"][0::3]
        ys = g["pos"][1::3]
        zs = g["pos"][2::3]

        pv = add_view(pos, 34962)
        uvv = add_view(uv, 34962) if uv else None
        cv = add_view(col, 34962) if col else None
        iv = add_view(idx, 34963)

        attributes = {"POSITION": len(accessors)}
        accessors.append({"bufferView": pv, "componentType": 5126, "count": count,
                          "type": "VEC3",
                          "min": [min(xs), min(ys), min(zs)],
                          "max": [max(xs), max(ys), max(zs)]})
        if uvv is not None:
            attributes["TEXCOORD_0"] = len(accessors)
            accessors.append({"bufferView": uvv, "componentType": 5126,
                              "count": count, "type": "VEC2"})
        if cv is not None:
            attributes["COLOR_0"] = len(accessors)
            accessors.append({"bufferView": cv, "componentType": 5121,
                              "count": count, "type": "VEC4", "normalized": True})
        accessors.append({"bufferView": iv, "componentType": idx_type,
                          "count": len(g["idx"]), "type": "SCALAR"})

        material = {"name": key,
                    "extensions": {"KHR_materials_unlit": {}},
                    "pbrMetallicRoughness": {
            "baseColorFactor": [to_linear(colour[0]), to_linear(colour[1]),
                                to_linear(colour[2]), colour[3] / 255.0],
            "metallicFactor": 0, "roughnessFactor": 1}}

        if flat:
            # The far view. Each surface takes the colour of the texture it
            # would have worn, which at this distance is indistinguishable and
            # costs nothing to download.
            average = textures.average(key)
            if average:
                base = material["pbrMetallicRoughness"]["baseColorFactor"]
                for n in range(3):
                    base[n] *= to_linear(average[n])
            materials.append(material)
            meshes.append({"primitives": [{
                "attributes": attributes,
                "indices": len(accessors) - 1,
                "material": len(materials) - 1}]})
            nodes.append({"mesh": len(meshes) - 1})
            continue

        texture_id, has_alpha = add_texture(key)
        if texture_id is not None:
            material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture_id}
            if has_alpha:
                # Fences, foliage and railings are a picture with holes in it.
                # Cutting the holes out is what the game does, and it is cheaper
                # than sorting every leaf back to front.
                material["alphaMode"] = "MASK"
                material["alphaCutoff"] = 0.5
                material["doubleSided"] = True

        if DOUBLE_SIDED:
            material["doubleSided"] = True

        materials.append(material)
        meshes.append({"primitives": [{
            "attributes": attributes,
            "indices": len(accessors) - 1,
            "material": len(materials) - 1}]})
        nodes.append({"mesh": len(meshes) - 1})

    if not meshes:
        return False

    gltf = {
        "asset": {"version": "2.0", "generator": "world3d-bake"},
        "extensionsUsed": ["KHR_materials_unlit"],
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    if images:
        gltf["images"] = images
        gltf["samplers"] = samplers
        gltf["textures"] = gl_textures

    body = json.dumps(gltf, separators=(",", ":")).encode()
    while len(body) % 4:
        body += b" "
    while len(blob) % 4:
        blob.append(0)

    with open(path, "wb") as f:
        total = 12 + 8 + len(body) + 8 + len(blob)
        f.write(b"glTF")
        f.write(struct.pack("<II", 2, total))
        f.write(struct.pack("<I", len(body)))
        f.write(b"JSON")
        f.write(body)
        f.write(struct.pack("<I", len(blob)))
        f.write(b"BIN\x00")
        f.write(bytes(blob))
    return True


def write_atlas_glb(groups, path, textures, content=None, border=None):
    """One tile as a single sheet and at most two meshes."""
    sheet, solid, masked, scale = merge_onto_sheet(groups, textures, content, border)
    if sheet is None:
        return None
    if not solid["idx"] and not masked["idx"]:
        return None

    blob = bytearray()
    views, accessors, meshes, materials, nodes = [], [], [], [], []

    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        blob.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        views.append(view)
        return len(views) - 1

    picture = io.BytesIO()
    sheet.save(picture, format="PNG", optimize=False)
    image_view = add_view(picture.getvalue(), None)

    for bucket, cutout in ((solid, False), (masked, True)):
        if not bucket["idx"]:
            continue

        count = len(bucket["pos"]) // 3
        xs, ys, zs = bucket["pos"][0::3], bucket["pos"][1::3], bucket["pos"][2::3]

        pv = add_view(struct.pack("<%df" % len(bucket["pos"]), *bucket["pos"]), 34962)
        uv = add_view(struct.pack("<%df" % len(bucket["uv"]), *bucket["uv"]), 34962)
        u2 = add_view(struct.pack("<%df" % len(bucket["uv2"]), *bucket["uv2"]), 34962)
        cv = add_view(bytes(bucket["col"]), 34962)

        if count <= 65535:
            idx = struct.pack("<%dH" % len(bucket["idx"]), *bucket["idx"])
            idx_type = 5123
        else:
            idx = struct.pack("<%dI" % len(bucket["idx"]), *bucket["idx"])
            idx_type = 5125
        iv = add_view(idx, 34963)

        first = len(accessors)
        accessors.append({"bufferView": pv, "componentType": 5126, "count": count,
                          "type": "VEC3",
                          "min": [min(xs), min(ys), min(zs)],
                          "max": [max(xs), max(ys), max(zs)]})
        accessors.append({"bufferView": uv, "componentType": 5126,
                          "count": count, "type": "VEC2"})
        accessors.append({"bufferView": u2, "componentType": 5126,
                          "count": count, "type": "VEC2"})
        accessors.append({"bufferView": cv, "componentType": 5121, "count": count,
                          "type": "VEC4", "normalized": True})
        accessors.append({"bufferView": iv, "componentType": idx_type,
                          "count": len(bucket["idx"]), "type": "SCALAR"})

        material = {
            "name": "masked" if cutout else "solid",
            "extensions": {"KHR_materials_unlit": {}},
            "pbrMetallicRoughness": {
                "baseColorFactor": [1, 1, 1, 1],
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0, "roughnessFactor": 1},
            # How much of a cell is picture rather than border. The viewer needs
            # it to wrap coordinates into the right part of the sheet.
            "extras": {"cellScale": scale},
        }
        if cutout:
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = 0.5
            material["doubleSided"] = True

        if DOUBLE_SIDED:
            material["doubleSided"] = True

        materials.append(material)
        meshes.append({"primitives": [{
            "attributes": {"POSITION": first, "TEXCOORD_0": first + 1,
                           "TEXCOORD_1": first + 2, "COLOR_0": first + 3},
            "indices": first + 4,
            "material": len(materials) - 1}]})
        nodes.append({"mesh": len(meshes) - 1})

    gltf = {
        "asset": {"version": "2.0", "generator": "world3d-bake"},
        "extensionsUsed": ["KHR_materials_unlit"],
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
        "images": [{"bufferView": image_view, "mimeType": "image/png"}],
        # The sheet must never wrap: wrapping is what the shader does, inside a
        # cell. Letting the sampler do it would read the neighbouring texture.
        "samplers": [{"wrapS": 33071, "wrapT": 33071,
                      "magFilter": 9729, "minFilter": 9987}],
        "textures": [{"source": 0, "sampler": 0}],
        "extras": {"cellScale": scale},
    }

    body = json.dumps(gltf, separators=(",", ":")).encode()
    while len(body) % 4:
        body += b" "
    while len(blob) % 4:
        blob.append(0)

    with open(path, "wb") as f:
        total = 12 + 8 + len(body) + 8 + len(blob)
        f.write(b"glTF")
        f.write(struct.pack("<II", 2, total))
        f.write(struct.pack("<I", len(body)))
        f.write(b"JSON")
        f.write(body)
        f.write(struct.pack("<I", len(blob)))
        f.write(b"BIN\x00")
        f.write(bytes(blob))
    return scale


def main():
    scene_path, index_path, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]

    tile = 512
    if "--tile" in sys.argv:
        tile = int(sys.argv[sys.argv.index("--tile") + 1])

    region = None
    if "--region" in sys.argv:
        region = [float(v) for v in sys.argv[sys.argv.index("--region") + 1].split(",")]

    # Written by world3d-textures.py, normally straight into the tile folder.
    texture_data = None
    texture_dir = out_dir
    if "--textures" in sys.argv:
        texture_dir = sys.argv[sys.argv.index("--textures") + 1]
    manifest = os.path.join(texture_dir, "textures.json")
    if os.path.exists(manifest):
        texture_data = json.load(open(manifest))
    else:
        print("  no textures.json found - surfaces will be left untextured")

    sheeted = "--atlas" in sys.argv

    # How big each texture is on the sheet, and how much of its own wrapped edge
    # surrounds it. The border decides how far the map can be zoomed out before
    # neighbouring textures start bleeding into each other, so the distant
    # version of the world is built with small textures and a wide border.
    if "--double-sided" in sys.argv:
        globals()["DOUBLE_SIDED"] = True

    content = atlas.CONTENT
    if "--content" in sys.argv:
        content = int(sys.argv[sys.argv.index("--content") + 1])
    border = atlas.BORDER
    if "--border" in sys.argv:
        border = int(sys.argv[sys.argv.index("--border") + 1])
    flat = "--flat" in sys.argv
    boxes = "--boxes" in sys.argv
    if boxes:
        flat = True

    min_size = 0.0
    if "--min-size" in sys.argv:
        min_size = float(sys.argv[sys.argv.index("--min-size") + 1])

    # Off by default: the site's tiles want the world as it is. The radar
    # passes this, via deploy/build-radar-tiles.sh.
    navigation = "--navigation" in sys.argv

    max_size = 1500.0
    if "--max-size" in sys.argv:
        max_size = float(sys.argv[sys.argv.index("--max-size") + 1])
    oversized = {}

    ground_height = 0.0
    if "--ground" in sys.argv:
        ground_height = float(sys.argv[sys.argv.index("--ground") + 1])

    scene = json.load(open(scene_path))
    index = json.load(open(index_path))["models"]
    cache = ModelCache(index)
    textures = Textures(texture_data, texture_dir)

    by_tile = {}
    dropped_props = dropped_lod = dropped_dupes = 0
    seen_spots = set()
    for inst in scene["instances"]:
        x, y = inst["p"][0], inst["p"][1]
        if region and not (region[0] <= x <= region[2] and region[1] <= y <= region[3]):
            continue
        # Whole objects, before anything is merged. Doing it here rather than
        # in either bake function means the tile's own instance count is
        # honest and --update rebuilds on the same basis.
        if navigation:
            name = inst["m"]
            if PROPS.search(name):
                dropped_props += 1
                continue
            if LOD.search(name) and not NOT_LOD.search(name):
                dropped_lod += 1
                continue
            # Avoid flickering from identical surfaces at identical depth.
            spot = (name.lower(), round(x, 2), round(y, 2),
                    round(inst["p"][2], 2))
            if spot in seen_spots:
                dropped_dupes += 1
                continue
            seen_spots.add(spot)
        key = (int(x // tile), int(y // tile))
        by_tile.setdefault(key, []).append(inst)
    if dropped_props or dropped_lod or dropped_dupes:
        print("  left out: %d street furniture and vegetation, %d stand-ins, "
              "%d duplicate placements"
              % (dropped_props, dropped_lod, dropped_dupes))

    os.makedirs(out_dir, exist_ok=True)

    # Adding to a map that already exists, rather than building it again.
    #
    # A full bake is twenty minutes and rewrites every tile, which is the wrong
    # shape of job for "one building was missing". In this mode a tile that has
    # already been built is left alone and its entry carried across, so rebaking
    # one corner of the map costs one corner of the map.
    adding = "--update" in sys.argv
    # --update on its own fills in what is missing. With --force it rebuilds
    # whatever is in scope as well - which, combined with --region, is how one
    # part of the map gets redone without touching the rest.
    forcing = "--force" in sys.argv

    previous = {}
    listing_path = os.path.join(out_dir, "tiles.json")
    if adding and os.path.exists(listing_path):
        with open(listing_path) as f:
            for record in json.load(f).get("tiles", []):
                previous[(record["x"], record["y"])] = record

    written = 0
    empty = 0
    kept = 0
    objects = 0
    listing = []
    stamp = int(time.time())

    for n, key in enumerate(sorted(by_tile), 1):
        tx, ty = key
        path = os.path.join(out_dir, "%d_%d.glb" % (tx, ty))

        if adding and not forcing and key in previous and os.path.exists(path):
            listing.append(previous[key])
            kept += 1
            continue

        instances = by_tile[key]
        groups = (bake_boxes(instances, cache, textures, min_size,
                             max_size, oversized, ground_height)
                  if boxes else
                  bake_tile(instances, cache, textures, min_size, flat))
        scale = None
        if sheeted:
            scale = write_atlas_glb(groups, path, textures, content, border)
            written_ok = scale is not None
        else:
            written_ok = write_glb(groups, path, textures, flat)

        if written_ok:
            written += 1
            objects += len(instances)
            record = {"x": tx, "y": ty, "n": len(instances),
                      "size": os.path.getsize(path),
                      # This tile's own version. Stamping the whole map at once
                      # would change every URL and make a browser re-fetch all
                      # of it because one tile moved.
                      "v": stamp}
            if scale:
                # How much of a texture sheet cell is picture rather than
                # border. Crowded tiles fall back to smaller cells, so this is
                # not the same for every tile and the viewer has to be told.
                record["cell"] = round(scale, 8)
                # How deep the viewer may let mipmapping go on this sheet.
                record["lod"] = atlas.safe_levels(border)
            listing.append(record)
        else:
            empty += 1
        if n % 25 == 0:
            print("    %d/%d tiles..." % (n, len(by_tile)), flush=True)

    # Tiles outside this run - a different region, or ones nothing touched -
    # stay in the list. Dropping them would delete them from the map.
    if adding:
        seen = {(t["x"], t["y"]) for t in listing}
        for key, record in previous.items():
            if key not in seen:
                listing.append(record)
                kept += 1

    with open(os.path.join(out_dir, "tiles.json"), "w") as f:
        # A stamp the viewer hangs off every tile request. Tiles keep their
        # names when they are rebuilt, so without this a browser happily serves
        # yesterday's geometry out of its cache and no amount of redeploying
        # changes what you see.
        json.dump({"tile": tile, "built": int(time.time()), "tiles": listing},
                  f, separators=(",", ":"))

    total = sum(t["size"] for t in listing)
    print("  %d tiles written, %d empty%s"
          % (written, empty, ", %d left as they were" % kept if kept else ""))
    print("  %d objects placed, %d models not found" % (objects, len(cache.misses)))
    rescued = textures.rescued
    if any(rescued.values()):
        print("  %d texture names were not in their own dictionary and were "
              "found in the pool: %d by a unique name, %d where every copy was "
              "the same picture, %d by the commonest dictionary%s"
              % (sum(rescued.values()), rescued["unique"], rescued["identical"],
                 rescued["commonest"],
                 "" if textures.pool_guess else " (guessing disabled)"))
    print("  %d texture names had no image" % len(textures.misses))
    if textures.misses and "--name-misses" in sys.argv:
        for name in sorted(textures.misses)[:40]:
            print("      %s" % name)
    if textures.unreadable:
        print("  %d images listed but missing from %s - e.g. %s"
              % (len(textures.unreadable), texture_dir,
                 sorted(textures.unreadable)[0]))
    # Anything that ended up on the atlas's plain white cell. This is the one
    # failure that is invisible in the output files and obvious on screen, so
    # it gets said every run rather than behind a flag.
    fallbacks = atlas.report()
    if fallbacks:
        print("  materials left untextured (drawn on the white cell):")
        print(fallbacks)
    if oversized:
        biggest = sorted(oversized.items(), key=lambda kv: -kv[1])[:5]
        print("  %d models too big to box, left out: %s"
              % (len(oversized), ", ".join("%s (%d units)" % b for b in biggest)))
    print("  %d MB total" % (total // 1048576))


if __name__ == "__main__":
    main()
