"""Lists every radio mast, transmitter and big dish placed on the map.

Reads the IDE files for model names, then every IPL the game loads: the text
IPLs named in data/gta.dat and the binary stream IPLs inside the IMG archives
it names. Power pylons and LOD models are left out.

    python find_masts.py "C:\\...\\Grand Theft Auto San Andreas" masts-found.csv
"""
import csv
import os
import re
import struct
import sys

NAME = re.compile(r"mast|antenna|aerial|radio|transmit|dish", re.I)
SKIP = re.compile(r"^lod|pylon|ringmaster", re.I)


def resolve(game, path):
    """Finds a gta.dat path on disk regardless of letter case."""
    current = game
    for part in path.replace("\\", "/").split("/"):
        try:
            match = [e for e in os.listdir(current) if e.lower() == part.lower()]
        except OSError:
            return None
        if not match:
            return None
        current = os.path.join(current, match[0])
    return current


def model_names(game):
    names = {}
    for root, _, files in os.walk(game):
        if "backups" in root.lower():
            continue
        for name in files:
            if not name.lower().endswith(".ide"):
                continue
            section = None
            for line in open(os.path.join(root, name), errors="ignore"):
                text = line.split("#")[0].strip()
                lower = text.lower()
                if lower in ("objs", "tobj", "anim"):
                    section = lower
                elif lower == "end":
                    section = None
                elif section and text:
                    fields = [f.strip() for f in text.split(",")]
                    try:
                        names[int(fields[0])] = fields[1]
                    except (ValueError, IndexError):
                        pass
    return names


def main(game, output):
    names = model_names(game)
    wanted = {i: n for i, n in names.items() if NAME.search(n) and not SKIP.search(n)}
    imgs, ipls = [], []
    for line in open(os.path.join(game, "data", "gta.dat"), errors="ignore"):
        text = line.strip()
        if text.upper().startswith("IMG "):
            imgs.append(text[4:].strip())
        elif text.upper().startswith("IPL "):
            ipls.append(text[4:].strip())

    found = []
    for ipl in ipls:
        path = resolve(game, ipl)
        if not path:
            continue
        section = None
        for line in open(path, errors="ignore"):
            text = line.strip()
            if text.lower() in ("inst", "end"):
                section = text.lower()
                continue
            if section != "inst" or not text or text.startswith("#"):
                continue
            fields = [f.strip() for f in text.split(",")]
            try:
                model = int(fields[0])
            except ValueError:
                continue
            if model in wanted:
                found.append((wanted[model], model, float(fields[3]), float(fields[4]),
                              float(fields[5]), int(fields[2]), os.path.relpath(path, game)))

    for img in imgs:
        path = resolve(game, img)
        if not path:
            continue
        with open(path, "rb") as file:
            if file.read(4) != b"VER2":
                continue
            count = struct.unpack("<I", file.read(4))[0]
            entries = [struct.unpack("<IHH24s", file.read(32)) for _ in range(count)]
            for offset, size, size2, raw in entries:
                entry = raw.split(b"\0")[0].decode("latin1")
                if not entry.lower().endswith(".ipl"):
                    continue
                file.seek(offset * 2048)
                data = file.read((size or size2) * 2048)
                if data[:4] != b"bnry":
                    continue
                count = struct.unpack_from("<I", data, 4)[0]
                start = struct.unpack_from("<I", data, 28)[0]
                for i in range(count):
                    x, y, z, _, _, _, _, model, interior, _ = struct.unpack_from(
                        "<7f3i", data, start + i * 40)
                    if model in wanted:
                        found.append((wanted[model], model, x, y, z, interior,
                                      os.path.basename(path) + ":" + entry))

    found.sort(key=lambda r: (r[0].lower(), r[2], r[3]))
    with open(output, "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["model", "id", "x", "y", "z", "interior", "source"])
        for row in found:
            writer.writerow([row[0], row[1], round(row[2], 1), round(row[3], 1),
                             round(row[4], 1), row[5], row[6]])
    print(f"{len(found)} placements written to {output}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "masts-found.csv")
