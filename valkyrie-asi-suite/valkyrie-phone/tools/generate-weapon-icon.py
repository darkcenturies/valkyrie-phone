"""Prepare our original phone drawing using stock SA weapon icon settings.

The stock hud.txd fist is 64x64 and DXT3, with a three-pixel square frame.
Retain the original handset pixels and replace only its heavier HQ square.
Requires Pillow with DXT3 encoding support (12.1 or newer).
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter


def handset_mask(image):
    # The frame is pure black. The convex hull of the coloured handset
    # encloses its black screen and details; eight source pixels retain
    # its original black contour. No generated/redrawn handset is used.
    points = sorted((x, y) for y in range(image.height) for x in range(image.width)
                    if (lambda p: p[3] > 128 and max(p[:3]) > 32)(image.getpixel((x, y))))
    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for point in points:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], point) <= 0:
            lower.pop()
        lower.append(point)
    for point in reversed(points):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], point) <= 0:
            upper.pop()
        upper.append(point)
    mask = Image.new("L", image.size)
    ImageDraw.Draw(mask).polygon(lower[:-1] + upper[:-1], fill=255)
    return mask.filter(ImageFilter.MaxFilter(17))


def hud_art(image):
    # Source artwork is 256px. Twelve source pixels become the stock
    # three-pixel frame at 64px. Its outer bounds remain in place.
    assert image.size == (256, 256)
    handset = image.copy()
    handset.putalpha(Image.composite(image.getchannel("A"), Image.new("L", image.size), handset_mask(image)))
    framed = Image.new("RGBA", image.size)
    ImageDraw.Draw(framed).rounded_rectangle((7, 20, 245, 237), radius=41,
                                             outline=(0, 0, 0, 255), width=12)
    framed.alpha_composite(handset)
    return framed.resize((64, 64), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    assets = Path(__file__).resolve().parents[1] / "assets/weapon-icon"
    image = hud_art(Image.open(assets / "phone-source.png").convert("RGBA"))
    image.save(assets / "phone.png")
    image.save(assets / "phone.dds", pixel_format="DXT3")
    print("Wrote phone.png and phone.dds: 64x64 DXT3, stock three-pixel square frame.")
