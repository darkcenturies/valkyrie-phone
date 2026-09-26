"""Writes valkyrie-phone.ini as the phone ships it: the sections the phone
writes itself on first start (kSections in valkyrie-phone/src/config.cpp),
in order, for release packages.

    python tools/phone-default-ini.py <out.ini>
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "valkyrie-phone", "src", "config.cpp")


def unescape(literal):
    out, i = [], 0
    while i < len(literal):
        c = literal[i]
        if c == "\\" and i + 1 < len(literal):
            n = literal[i + 1]
            out.append({"r": "\r", "n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    text = open(SOURCE, encoding="utf-8").read()
    start = text.index("const Section kSections[] = {")
    end = text.index("\n};", start)
    body = text[start:end]
    # Each entry: {"Name", "text" "text" ...},
    literals = re.finditer(r'"((?:[^"\\]|\\.)*)"|([{}])', body)
    sections, current, depth = [], None, 0
    for m in literals:
        if m.group(2) == "{":
            depth += 1
            if depth == 2:
                current = []
        elif m.group(2) == "}":
            if depth == 2 and current is not None:
                sections.append(current)
                current = None
            depth -= 1
        elif current is not None:
            current.append(unescape(m.group(1)))
    out = "".join("".join(entry[1:]) for entry in sections)
    with open(sys.argv[1], "w", encoding="ascii", newline="") as f:
        f.write(out)
    print(f"wrote {sys.argv[1]}: {len(sections)} sections, {len(out)} bytes")


if __name__ == "__main__":
    main()
