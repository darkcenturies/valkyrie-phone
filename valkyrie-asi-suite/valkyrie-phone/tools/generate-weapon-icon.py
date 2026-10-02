"""Prepare our original phone drawing using stock SA weapon icon settings.

The stock hud.txd fist is 64x64 and DXT3. Keep our original artwork; resize
with area filtering and compress its alpha/colour with the same DXT format.
Requires Pillow with DXT3 encoding support (12.1 or newer).
"""
from pathlib import Path
from PIL import Image

if __name__ == "__main__":
    assets = Path(__file__).resolve().parents[1] / "assets/weapon-icon"
    image = Image.open(assets / "phone-source.png").convert("RGBA")
    image = image.resize((64, 64), Image.Resampling.LANCZOS)
    image.save(assets / "phone.png")
    image.save(assets / "phone.dds", pixel_format="DXT3")
    print("Wrote phone.png and phone.dds: 64x64, stock SA DXT3 compression.")
