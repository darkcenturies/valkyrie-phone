#!/usr/bin/env python3
"""
world3d-textures.py -- Give the 3D map its surfaces.

The tiles already know which texture every face wants, by name. What they cannot
know is which dictionary that name lives in: a model does not carry its own
textures, it is assigned a dictionary in one of the game's data files, and the
same name means different pictures in different dictionaries. There are eleven
textures called "white".

    id, modelname, txdname, drawdistance, flags

So this reads that assignment out of every .ide, works out which dictionaries
the region being baked actually needs, and writes each of their textures out as
an ordinary image a browser can load.

    world3d-textures.py GAME_DIR INDEX.json SCENE.json OUT_DIR
        [--region x0,y0,x1,y1] [--max 256]

Opaque textures are written as JPEG and ones with transparency as PNG, because
half the world is brick and the other half is chain-link fence and only the
second kind needs an alpha channel.
"""
import importlib.util
import json
import os
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("txdread", os.path.join(_here, "txd-read.py"))
txdread = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(txdread)

# Sections of an .ide whose second and third fields are the model and its
# texture dictionary. The rest of each line differs section to section and none
# of it matters here.
MODEL_SECTIONS = ("objs", "tobj", "anim", "hier", "cars", "peds", "weap")

MAX_SIZE = 256


def read_ide(path):
    """model -> texture dictionary, plus any dictionary-inherits-dictionary lines."""
    models, parents = {}, {}
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
                # A section name is a short word on its own line. Testing for
                # letters alone would miss 2dfx and swallow its contents.
                if "," not in low and len(low) <= 5:
                    section = low
                    continue

                parts = [p.strip() for p in line.split(",")]

                # txdp is the game's own fallback rule: look in this dictionary,
                # and if the texture is not there look in its parent.
                if section == "txdp" and len(parts) >= 2:
                    parents[parts[0].lower()] = parts[1].lower()
                    continue

                if section in MODEL_SECTIONS and len(parts) >= 3:
                    models[parts[1].lower()] = parts[2].lower()
    except OSError:
        pass
    return models, parents


def model_map(game):
    """Every model's texture dictionary, as the game resolves it."""
    models, parents = {}, {}
    files = 0
    for base, _dirs, names in os.walk(game):
        if os.sep + "backups" in base.lower():
            continue
        for name in names:
            if not name.lower().endswith(".ide"):
                continue
            files += 1
            m, p = read_ide(os.path.join(base, name))
            models.update(m)
            parents.update(p)
    return models, parents, files


def safe(name):
    """A texture name that is also a usable file name."""
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in name)[:64] or "unnamed"


def export(record, out_dir, folder, max_size):
    """Every texture in one dictionary, written out as images."""
    from PIL import Image

    try:
        with open(record["src"], "rb") as f:
            f.seek(record["off"])
            data = f.read(record["len"])
        textures = txdread.read_txd(data)
    except OSError:
        return {}, 0, 0

    written, failed = {}, 0
    target = os.path.join(out_dir, "tex", folder)

    for tex in textures:
        image = txdread.to_image(tex)
        if image is None:
            failed += 1
            continue

        # The game ships textures up to 1024 across. Nothing on a map seen from
        # this far out is improved by that, and the download is.
        if max(image.size) > max_size:
            image.thumbnail((max_size, max_size), Image.LANCZOS)

        alpha = image.getchannel("A")
        low, _high = alpha.getextrema()
        transparent = low < 250

        os.makedirs(target, exist_ok=True)
        base = safe(tex["name"])
        if transparent:
            name = base + ".png"
            image.save(os.path.join(target, name), optimize=True)
        else:
            name = base + ".jpg"
            image.convert("RGB").save(os.path.join(target, name),
                                      quality=82, optimize=True)

        # The whole texture boiled down to one colour. Seen from far enough away
        # a wall is its average colour and nothing else, so the far view can be
        # drawn from these alone and carry no images at all.
        average = image.convert("RGB").resize((1, 1), Image.LANCZOS).getpixel((0, 0))

        written[tex["name"].lower()] = {"f": "tex/%s/%s" % (folder, name),
                                        "a": 1 if transparent else 0,
                                        "c": list(average)}
    return written, len(written), failed


def main():
    game, index_path, scene_path, out_dir = sys.argv[1:5]

    max_size = MAX_SIZE
    if "--max" in sys.argv:
        max_size = int(sys.argv[sys.argv.index("--max") + 1])

    region = None
    if "--region" in sys.argv:
        region = [float(v) for v in sys.argv[sys.argv.index("--region") + 1].split(",")]

    print("  reading the data files...")
    models, parents, ide_files = model_map(game)
    print("  %d data files read, %d models assigned a dictionary, %d inherit"
          % (ide_files, len(models), len(parents)))

    index = json.load(open(index_path))["textures"]
    scene = json.load(open(scene_path))

    # Only the dictionaries the region actually uses.
    wanted, unknown = set(), set()
    for inst in scene["instances"]:
        x, y = inst["p"][0], inst["p"][1]
        if region and not (region[0] <= x <= region[2] and region[1] <= y <= region[3]):
            continue
        name = inst["m"].lower()
        txd = models.get(name)
        if txd is None:
            unknown.add(name)
            continue
        wanted.add(txd)
        seen = 0
        while txd in parents and seen < 8:
            txd = parents[txd]
            wanted.add(txd)
            seen += 1

    print("  %d texture dictionaries needed, %d models have none listed"
          % (len(wanted), len(unknown)))

    manifest, missing = {}, []
    total = failed = 0
    for n, name in enumerate(sorted(wanted), 1):
        record = index.get(name)
        if record is None:
            missing.append(name)
            continue
        written, count, bad = export(record, out_dir, safe(name), max_size)
        for tex, entry in written.items():
            manifest["%s/%s" % (name, tex)] = entry
        total += count
        failed += bad
        if n % 100 == 0:
            print("    %d/%d dictionaries..." % (n, len(wanted)), flush=True)

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "textures.json"), "w") as f:
        json.dump({"models": models, "parents": parents, "files": manifest},
                  f, separators=(",", ":"))

    size = 0
    for base, _dirs, names in os.walk(os.path.join(out_dir, "tex")):
        size += sum(os.path.getsize(os.path.join(base, n)) for n in names)

    print("  %d textures written, %d could not be read" % (total, failed))
    print("  %d dictionaries not found in any archive" % len(missing))
    print("  %d MB of images" % (size // 1048576))


main()
