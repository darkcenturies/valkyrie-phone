"""Apply the Harry Mason / Project Silent Hill phone preset, preserving other keys.
Usage: python configure-silent-hill-phone.py phone.ini [--atmosphere atmosphere.ini]
Existing files are backed up before changes. No binaries are installed here.
Contact references: https://www.silenthillmemories.net/sh_shattered_memories/phone_numbers_en.htm
"""
from pathlib import Path
import argparse
import datetime
import re
import shutil

CONTACTS = {"Home (Cheryl)": "5554663", "Cybil": "5552925", "Michelle": "5553587", "Dahlia": "5557399"}

def section(data, name, values, replace=False):
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    pattern = rb"^\[" + re.escape(name.encode()) + rb"\][ \t]*\r?\n(.*?)(?=^\[|\Z)"
    match = re.search(pattern, data, re.M | re.S | re.I)
    body = b"" if replace or not match else match.group(1)
    for key, value in values.items():
        line = key.encode() + b"=" + str(value).encode("ascii")
        pattern_key = rb"^" + re.escape(key.encode()) + rb"[ \t]*=[^\r\n]*"
        if re.search(pattern_key, body, re.M | re.I):
            body = re.sub(pattern_key, lambda _: line, body, flags=re.M | re.I)
        else:
            if body and not body.endswith(b"\n"): body += nl
            body += line + nl
    if match:
        return data[:match.start(1)] + body + data[match.end(1):]
    return data + nl + b"[" + name.encode() + b"]" + nl + body

def phone(data):
    for name, values in {
        "Phone": {"Profile": "SilentHill", "Skin": "ShatteredMemories", "Height": "0.70", "Model3D": "1", "ScreenEffect": "0.12"},
        "Apps": {"Order": "Contacts,Phone,Photos,Maps,Text,Camera,Settings,Notes,Flashlight", "Dock": ""},
        "Labels": {"Contacts": "Phone book", "Phone": "Call", "Photos": "Pictures", "Maps": "Map", "Text": "Messages", "Notes": "Journal"},
        "Model": {"Reflections": "1", "Shine": "0.25"},
        "Features": {"Scratches": "1", "ScratchLook": "Subtle", "Cracks": "0", "Blood": "0", "GlassShine": "1"},
        "Look": {"InkWidth": "0.0"},
        "Services": {k: "" for k in ("Emergency", "NonEmergency", "Hotline", "Hotel", "Save", "Trainer", "Pizza", "Burger", "Chicken")},
    }.items(): data = section(data, name, values)
    data = section(data, "SilentHillContacts", CONTACTS)
    return data

def atmosphere(data):
    return section(data, "Item.-8", {"Enabled": 1, "Usable": 1, "Name": "Harry's phone", "Description": "A phone, a camera, and the numbers I still remember.", "Examine": "Cheryl's number is still here. I should try it again.", "IconPack": "items", "Icon": "phone"})

def update(path, fn, backup=True):
    path = Path(path)
    before = path.read_bytes()
    after = fn(before)
    if before == after: return
    backup_path = path.with_name(path.name + ".before-silent-hill-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".bak")
    if backup: shutil.copy2(path, backup_path)
    path.write_bytes(after)
    print("Configured", path)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phone_ini")
    parser.add_argument("--atmosphere")
    parser.add_argument("--no-backup", action="store_true", help="For freshly generated package INIs only")
    args = parser.parse_args()
    update(args.phone_ini, phone, not args.no_backup)
    if args.atmosphere: update(args.atmosphere, atmosphere, not args.no_backup)
