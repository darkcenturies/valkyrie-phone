"""Generate the phone weapon HUD artwork on a 16-pixel grid.

Run from any directory. Flat silver bands and a heavy black outline match
SA's chunky HUD artwork. No antialiasing or resampling is used.
"""
from pathlib import Path
from PIL import Image

PALETTE = {
    ".": (0, 0, 0, 0),
    "#": (0, 0, 0, 255),
    "W": (235, 235, 220, 255),
    "S": (160, 164, 155, 255),
    "G": (65, 70, 65, 255),
}
PIXELS = (
    "......#####.....",
    ".....#######....",
    ".....##WWW###...",
    "....##W##WW##...",
    "....##WWWWW###..",
    "....##W####S##..",
    "...##WW#G##S##..",
    "...##W#####S##..",
    "...##W####S##...",
    "..##WW####S##...",
    "..##W#####S##...",
    "..##W####S##....",
    "..##WW#WWS##....",
    "...##WWWS##.....",
    "....######......",
    ".....####.......",
)

if __name__ == "__main__":
    assert len(PIXELS) == 16 and all(len(row) == 16 for row in PIXELS)
    image = Image.new("RGBA", (16, 16))
    image.putdata([PALETTE[pixel] for row in PIXELS for pixel in row])
    output = Path(__file__).resolve().parents[1] / "assets/weapon-icon/phone.png"
    image.save(output)
    print(f"Wrote {output.name}: 16x16, hard pixel edges.")
