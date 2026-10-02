#!/usr/bin/env python3
"""
gen-world-scene.py -- Read the world's layout out of the game, for the 3D map.

Every building, wall and prop in the game is an entry in an IPL file: which
model to use, where to put it, and how it is turned.

There are two kinds, and both matter. The text ones under data/maps are the
readable kind:

    id, modelname, interior, x, y, z, rx, ry, rz, rw, lod

The rest of the world - most of it - is in binary streams packed inside the
archives, which the game loads a piece at a time as you drive. Those name their
model by number rather than by name, so the .ide files have to be read first to
turn a number back into a model. Reading only the text files gets you a city
made almost entirely of the simplified shapes the game shows at a distance,
because that is what the text files mostly place.

Two things are dropped. Interiors live in the same files but are stacked far
above the map in their own dimension, and drawing them would put hotel rooms in
the sky. Low-detail stand-ins are dropped as well: each is a crude copy of a
real building that is also in the list, and drawing both puts a box over the
thing it stands in for.

    gen-world-scene.py GAME_DIR OUT.json
"""
import json
import os
import struct
import sys
from collections import Counter

# Anything past this is a separate interior stacked above the world.
INTERIOR_CEILING = 900.0

# Except the Kickstart stadium, which is kept.
#
# Interiors sit about a kilometre above the map, out over the bay, and are
# dropped with the rest of them - but this one is a landmark people know, so
# the box it occupies is excepted.
#
# By position rather than by model name: the pieces inside it are called things
# like "wall1", "bit", "steps" and "rings", which are used in other interiors
# too, and matching on those names would drag half of them back in. The box is
# drawn a little wider than the stadium so nothing on its edge is clipped.
#
# The other three stadium interiors in the same file - the oval, the dirt track
# and the bowl - stay out.
# Below this is under the world rather than in it.
#
# Mappers park an object they want gone by dropping it far below the ground
# rather than deleting it - a hundred and twenty trees sit at exactly -300, and
# a set of lights at -1022. They are not meant to be seen, and over water or
# through a gap in the terrain they are. The sea floor is the deepest thing
# that genuinely belongs, at about -120, so the line goes under that.
WORLD_FLOOR = -150.0

STADIUM_BOX = (-1520.0, 1520.0, -1320.0, 1690.0)    # x0, y0, x1, y1
STADIUM_HEIGHT = (1000.0, 1120.0)


def kept_interior(pos):
    """Is this one of the few interiors we show anyway?"""
    x, y, z = pos
    return (STADIUM_BOX[0] <= x <= STADIUM_BOX[2]
            and STADIUM_BOX[1] <= y <= STADIUM_BOX[3]
            and STADIUM_HEIGHT[0] <= z <= STADIUM_HEIGHT[1])

SECTOR = 2048

# Sections of an .ide whose first two fields are a model's number and its name.
MODEL_SECTIONS = ("objs", "tobj", "anim", "hier", "cars", "peds", "weap")

# A binary IPL: "bnry", then counts, then where the placements start.
BINARY_HEADER = 32
BINARY_ENTRY = 40


def model_names(game):
    """Model number -> model name, which is what the binary files refer to."""
    names = {}
    section = None
    files = 0

    for base, _dirs, entries in os.walk(game):
        if os.sep + "backups" in base.lower():
            continue
        for entry in entries:
            if not entry.lower().endswith(".ide"):
                continue
            files += 1
            try:
                with open(os.path.join(base, entry), "r", encoding="latin-1") as f:
                    for raw in f:
                        line = raw.split("#")[0].strip()
                        if not line:
                            continue
                        low = line.lower()
                        if low == "end":
                            section = None
                            continue
                        if "," not in low and len(low) <= 5:
                            section = low
                            continue
                        if section not in MODEL_SECTIONS:
                            continue
                        parts = [p.strip() for p in line.split(",")]
                        if len(parts) >= 2 and parts[0].isdigit():
                            names[int(parts[0])] = parts[1]
            except OSError:
                continue
    return names, files


def mark_stand_ins(out):
    """Flag every placement that is only a distant stand-in for another one.

    The last field of a placement is the position, within this same file, of the
    simplified shape the game swaps in at a distance. Anything pointed at that
    way is a low-detail copy of something else in the list.

    Positions are counted including the placements we could not read, which is
    why those are kept as gaps rather than dropped. Leave one out and every
    index after it names the wrong placement - which deletes real roads and
    buildings instead of the stand-ins it was meant to name.
    """
    pointed_at = {e["lod"] for e in out if e and e["lod"] >= 0}

    # Only the hand-written layout files count positions this way. The streamed
    # ones - the bulk of the map - reuse the same field for something else, and
    # their numbers land on lampposts, roads and houses. Taking them at face
    # value deletes real geometry and leaves holes in the ground.
    #
    # Rather than guess from the filename, check: if the things being named are
    # really stand-ins they will be named for it, and if they are not, the
    # numbers mean something we do not understand and are ignored.
    named = [out[i] for i in pointed_at if 0 <= i < len(out) and out[i]]
    stand_in_like = sum(1 for e in named
                        if e["model"].lower().startswith("lod"))
    trust_positions = bool(named) and stand_in_like * 2 >= len(named)

    for n, entry in enumerate(out):
        if entry is None:
            continue
        # Whatever the positions say, a stand-in gives itself away by name.
        entry["is_lod"] = (entry["model"].lower().startswith("lod")
                           or (trust_positions and n in pointed_at))
    return [e for e in out if e is not None]


def text_instances(path):
    """The inst section of one readable IPL."""
    out = []
    section = None
    try:
        with open(path, "r", encoding="latin-1") as f:
            for raw in f:
                line = raw.split("#")[0].strip()
                if not line:
                    continue

                low = line.lower()
                if low == "end":
                    section = None
                    continue
                if low in ("inst", "cull", "zone", "occl", "grge", "enex",
                           "pick", "jump", "tcyc", "auzo", "mult", "cars", "path"):
                    section = low
                    continue
                if section != "inst":
                    continue

                parts = [p.strip() for p in line.split(",")]
                if len(parts) < 11 or not parts[0].isdigit():
                    out.append(None)
                    continue

                try:
                    out.append({
                        "model": parts[1],
                        "interior": int(parts[2]) & 0xff,
                        "pos": [float(parts[3]), float(parts[4]), float(parts[5])],
                        "rot": [float(parts[6]), float(parts[7]),
                                float(parts[8]), float(parts[9])],
                        "lod": int(parts[10]),
                    })
                except ValueError:
                    out.append(None)
                    continue
    except OSError:
        pass
    return mark_stand_ins(out)


def binary_instances(blob, names):
    """The placements inside one packed IPL stream.

    Forty bytes each: where it is, how it is turned, which model, which
    interior, and which entry stands in for it at a distance.
    """
    out = []
    if len(blob) < BINARY_HEADER or blob[:4] != b"bnry":
        return out

    count, = struct.unpack_from("<I", blob, 4)
    offset, = struct.unpack_from("<I", blob, 28)

    for i in range(count):
        at = offset + i * BINARY_ENTRY
        if at + BINARY_ENTRY > len(blob):
            break
        (px, py, pz, rx, ry, rz, rw,
         model_id, interior, lod) = struct.unpack_from("<7f3i", blob, at)

        name = names.get(model_id)
        if name is None:
            out.append(None)          # a gap, so the positions after it still line up
            continue

        out.append({
            "model": name,
            "interior": interior & 0xff,
            "pos": [px, py, pz],
            "rot": [rx, ry, rz, rw],
            "lod": lod,
        })
    return mark_stand_ins(out)


def archive_streams(path):
    """Name -> contents for every IPL packed into one archive."""
    out = {}
    try:
        with open(path, "rb") as f:
            head = f.read(8)
            if head[:4] != b"VER2":
                return out
            count = struct.unpack_from("<I", head, 4)[0]
            table = f.read(count * 32)

            for i in range(count):
                at = i * 32
                if at + 32 > len(table):
                    break
                start, streaming, size = struct.unpack_from("<IHH", table, at)
                name = table[at + 8:at + 32].split(b"\0")[0].decode("latin-1").lower()
                if not name.endswith(".ipl"):
                    continue
                f.seek(start * SECTOR)
                out[name] = f.read((streaming or size) * SECTOR)
    except OSError:
        pass
    return out


def main():
    game, out_path = sys.argv[1], sys.argv[2]

    names, ide_files = model_names(game)

    found = []
    text_files = 0
    seen = set()

    for base, _dirs, entries in os.walk(game):
        if os.sep + "backups" in base.lower():
            continue
        for entry in entries:
            if entry.lower().endswith(".ipl"):
                text_files += 1
                seen.add(entry.lower())
                found.extend(text_instances(os.path.join(base, entry)))

    from_text = len(found)

    archives = 0
    streams = 0
    for base, _dirs, entries in os.walk(game):
        if os.sep + "backups" in base.lower():
            continue
        for entry in entries:
            if not entry.lower().endswith(".img"):
                continue
            archives += 1
            for name, blob in archive_streams(os.path.join(base, entry)).items():
                # A loose file of the same name has already replaced this one,
                # the same way the game would.
                if name in seen:
                    continue
                seen.add(name)
                streams += 1
                found.extend(binary_instances(blob, names))

    # The stand-ins are normally what we throw away. Asked for on their own they
    # become the far-distance version of the map: they are exactly the shapes
    # the game itself shows when a building is too far off to draw properly.
    want_stand_ins = "--stand-ins" in sys.argv

    # Area 13 is shared by every area, including the exterior: GTA's
    # CEntity::IsInCurrentArea accepts it alongside the current area.
    # Excluding it removes real roads and ground (for example Lae2_roads28
    # and lae2_ground08 beside the new-game spawn). The instance type
    # packs stream/tunnel flags above the low-byte area code.
    # Stock exterior placements and the retained stadium landmark.
    outdoor = [i for i in found
               if ((i["interior"] in (0, 13, 255) and i["pos"][2] < INTERIOR_CEILING)
                   or kept_interior(i["pos"]))
               and i["pos"][2] > WORLD_FLOOR
               and i["is_lod"] == want_stand_ins]

    stand_ins = sum(1 for i in found if i["is_lod"])
    models = Counter(i["model"].lower() for i in outdoor)

    scene = {
        "cell": 64,
        "instances": [
            {"m": i["model"], "p": [round(v, 3) for v in i["pos"]],
             "r": [round(v, 5) for v in i["rot"]]}
            for i in outdoor
        ],
    }
    with open(out_path, "w") as f:
        json.dump(scene, f, separators=(",", ":"))

    xs = [i["pos"][0] for i in outdoor]
    ys = [i["pos"][1] for i in outdoor]

    print(f"  {ide_files} data files read, {len(names)} models numbered")
    print(f"  {text_files} readable layout files gave {from_text} placements")
    print(f"  {streams} packed layout streams in {archives} archives gave "
          f"{len(found) - from_text}")
    print(f"  {len(found)} placements in all, {stand_ins} of them stand-ins")
    print(f"  {len(outdoor)} kept"
          f"{' (the stand-ins, for the far view)' if want_stand_ins else ''}")
    print(f"  {len(models)} different models used")
    print(f"  spans x {min(xs):.0f} .. {max(xs):.0f}   y {min(ys):.0f} .. {max(ys):.0f}")
    print(f"  the twenty most used account for "
          f"{sum(n for _, n in models.most_common(20)) * 100 // len(outdoor)}% of the world")
    print(f"  written to {out_path} ({os.path.getsize(out_path) // 1024} KB)")


if __name__ == "__main__":
    main()
