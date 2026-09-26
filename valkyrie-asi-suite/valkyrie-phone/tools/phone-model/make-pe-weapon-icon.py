"""The phone's HUD weapon icon in Project Eagle's own style: the phone as a
shaded silhouette in the blue-to-white of Eagle's weapon icons, with its dark
drop shadow, leaning out of the top right of Eagle's star badge.

    python make-pe-weapon-icon.py <render.png> <eagle-icons-dir> <out.png>

<render.png> is preview-dff.py's "pe-icon" render of the phone (shape and
shading only). <eagle-icons-dir> holds Eagle's own weapon icons as PNGs
(pulled from its weapons.img): the badge is recovered from them (the
colour most of them share at each pixel - the weapons move, the badge does
not), and so is the palette (each brightness of their weapons' shading,
and the colour Eagle gives it).
"""
import glob
import sys

import numpy as np
from PIL import Image, ImageFilter

RENDER, ICONS, OUT = sys.argv[1:4]
S = 256

stack = np.stack([np.array(Image.open(f).convert("RGBA")) for f in glob.glob(ICONS + "/*.png")
                  if Image.open(f).size == (S, S)]).astype(np.int32)

# The badge: at each pixel, the colour most icons share.
q = stack >> 3
key = ((q[..., 0] * 32 + q[..., 1]) * 32 + q[..., 2]) * 32 + q[..., 3]
badge = np.zeros((S, S, 4), np.uint8)
for y in range(S):
    for x in range(S):
        k = key[:, y, x]
        vals, counts = np.unique(k, return_counts=True)
        badge[y, x] = stack[k == vals[counts.argmax()], y, x].mean(0).round()

# The palette: the weapons' own pixels (far from the badge, fully opaque,
# blue-dominant as Eagle shades them), averaged by brightness.
diff = np.abs(stack[..., :3] - badge[None, ..., :3].astype(np.int32)).sum(-1)
pix = stack[(diff > 90) & (stack[..., 3] > 250)][:, :3]
pix = pix[(pix[:, 2] > pix[:, 0] + 25) & (pix[:, 2] > 90)]
lum = (0.3 * pix[:, 0] + 0.59 * pix[:, 1] + 0.11 * pix[:, 2])
lo, hi = np.percentile(lum, 2), np.percentile(lum, 98)
lut = np.zeros((256, 3))
for i in range(256):
    target = lo + (hi - lo) * i / 255
    near = np.abs(lum - target) < (hi - lo) / 40
    lut[i] = pix[near].mean(0) if near.any() else lut[i - 1]

# The phone: cut to its outline and sized to lean out of the badge's top
# right as the game's phones do - its foot inside the badge, its top past
# the corner.
render = Image.open(RENDER).convert("RGBA")
render = render.crop(render.getchannel("A").getbbox())
height = 236
render = render.resize((max(1, int(render.width * height / render.height)), height), Image.LANCZOS)
r = np.array(render).astype(np.float32)
# Eagle's weapons are simple solid shapes: the render's light is kept only
# in its broad strokes (blurred well past its screen, button and edges), so
# the shape reads and the detail does not.
broad = render.convert("L").filter(ImageFilter.GaussianBlur(14))
rl = np.array(broad).astype(np.float32)
inside = r[..., 3] > 128
rlo, rhi = np.percentile(rl[inside], 1), np.percentile(rl[inside], 99)
shade = np.clip((rl - rlo) / max(rhi - rlo, 1.0), 0, 1)
# Eagle's weapons run pale at the top to deep blue at the foot, whatever
# their shape; the render's own light is laid over that, and its darkest
# parts (the screen) kept off the bottom of the palette.
down = np.linspace(1.0, 0.0, r.shape[0])[:, None]
t = np.clip(0.75 * down + 0.25 * shade - 0.02, 0, 1)
coloured = lut[(t * 255).astype(int)]
phone = Image.fromarray(np.dstack([coloured, r[..., 3]]).clip(0, 255).astype(np.uint8), "RGBA")
x, y = S - phone.width - 6, S - phone.height - 8
icon = Image.fromarray(badge, "RGBA")
# The thick dark edge Eagle's weapons have: a crisp shadow thrown down and
# to the right, the top and left edges left to catch the light.
pad = 12
mask = Image.new("L", (phone.width + 2 * pad, phone.height + 2 * pad), 0)
mask.paste(phone.getchannel("A"), (pad, pad))
edge = mask.filter(ImageFilter.MaxFilter(3))
dark = Image.new("RGBA", mask.size, (6, 8, 30, 0))
dark.putalpha(edge)
for step in range(1, 8):  # swept out down and right, a solid band
    icon.alpha_composite(dark, (x - pad + step, y - pad + step))
icon.alpha_composite(phone, (x, y))
icon.save(OUT)
print("wrote", OUT)
