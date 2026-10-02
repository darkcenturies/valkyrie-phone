"""Draws the phone's artwork and writes it to ../assets/generated.

The phone is the first iPhone's shape (2007: one big screen, one round home
button), drawn the way San Andreas draws its own interface art: a heavy black
outline around flat, lightly shaded colour, like the radar icons and the HUD.
The app icons are drawn the way the radar icons in models/hud.txd are: 64
pixels square, hard stepped edges, a thick black outline, flat saturated
colour in a few hard bands, a white glint and a little grain.

Where the game already has the right picture, the plugin loads the game's own
at runtime instead of drawing one here: the camera and spanner radar icons
(models/hud.txd) and the mouse cursor (models/fronten_pc.txd). The ones drawn
here for those are only fallbacks. What SP-RP made for its phone is reused as
it is - the 27 wallpapers, the battery and the contact glyph - read from
../assets/sprp and ../assets/wallpapers, decoded from the server's phone.txd
and phone_nice.txd.

Run it again after changing a drawing; tools/pack-phone-txd.ps1 turns the
output into valkyrie-phone.txd during the build. It also writes
../src/phone_art.h, the positions on the body texture that the plugin needs.

    python generate-phone-art.py
"""
import math
import os
import argparse

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
OUT = os.path.join(ASSETS, "generated")
SS = 4  # supersampling factor
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--apps-only', action='store_true', help='Regenerate only home-screen app icons.')
args = parser.parse_args()

# Regenerate the base artwork without deleting independently generated skins.
os.makedirs(OUT, exist_ok=True)
for old in os.listdir(OUT):
    if not args.apps_only and not old.startswith("sm_"):
        os.remove(os.path.join(OUT, old))

BLACK = (0, 0, 0, 255)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def vgradient(size, top, bottom):
    w, h = size
    col = Image.new("RGBA", (1, h))
    px = col.load()
    for y in range(h):
        px[0, y] = lerp(top, bottom, y / max(1, h - 1))
    return col.resize((w, h), Image.NEAREST)


def pow2(v):
    p = 1
    while p < v:
        p *= 2
    return p


def finish(img, size, name):
    """Save at the next power-of-two size up; the plugin draws each texture
    back at its designed aspect, which undoes the stretch."""
    img = img.resize((pow2(size[0]), pow2(size[1])), Image.LANCZOS)
    img.save(os.path.join(OUT, name + ".png"))


def fill_mask(mask, colour_or_image):
    if isinstance(colour_or_image, Image.Image):
        layer = colour_or_image.copy()
    else:
        layer = Image.new("RGBA", mask.size, colour_or_image)
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), mask))
    return layer


def dilate(mask, radius):
    """Grow a mask by about `radius` pixels, following its shape - the way
    the radar icons' black edge does."""
    grown = mask.filter(ImageFilter.GaussianBlur(radius * 0.5))
    return grown.point(lambda v: 255 if v > 8 else 0)


# ---------------------------------------------------------------------------
# The body
# ---------------------------------------------------------------------------
# Phone units, 4 to the millimetre. The handset is the iFruit, laid out the
# way the phone it copies is: the 320 x 480 screen, an earpiece and the iFruit
# mark above it, the round home button below it; on its left edge the ring
# switch and the two volume buttons, on its right edge the sleep button. The
# texture leaves SIDE units either side of the body for the buttons to stand
# out into.
PU_W, TOP, SIDE = 244.0, 4.0, 7.0
TEX_PU_W = PU_W + 2 * SIDE
BODY_TEX = (1024, 2048)  # twice the size it is drawn at, so it is never magnified
K = BODY_TEX[0] / TEX_PU_W

GLASS_X = 14.0
GLASS_W = PU_W - 2 * GLASS_X
SCREEN = (GLASS_X, TOP + 62.0, GLASS_X + GLASS_W, TOP + 62.0 + GLASS_W * 480.0 / 320.0)
HOME = (PU_W / 2, SCREEN[3] + 34.0, 21.0)  # centre and radius
BODY_H = HOME[1] + HOME[2] + 18.0 - TOP
# The side buttons: (top, bottom) along the edge.
RING = (TOP + 66.0, TOP + 80.0)
VOL_UP = (TOP + 96.0, TOP + 126.0)
VOL_DOWN = (TOP + 134.0, TOP + 164.0)
SLEEP = (TOP + 88.0, TOP + 132.0)
BUTTON_OUT = 4.2  # how far a button stands out of the edge
EAR = (PU_W / 2 - 24.0, TOP + 44.0, PU_W / 2 + 24.0, TOP + 50.0)
RADAR_LOGO = os.path.join(HERE, "..", "..", "valkyrie-radar", "src", "radar_logo.h")


def ifruit_mark():
    """The iFruit mark, from valkyrie-radar's own mask of it, so both carry
    the same one."""
    import re
    text = open(RADAR_LOGO).read()
    size = int(re.search(r"kRadarLogoSize = (\d+)", text).group(1))
    body = text[text.index("{") + 1:text.rindex("}")]
    return Image.frombytes("L", (size, size), bytes(int(v) for v in re.findall(r"\d+", body)))


def rounded_box(px, py, cx, cy, hw, hh, r):
    """Signed distance to a rounded rectangle, negative inside, and the
    outward direction (x right, y down) at the nearest edge."""
    ax, ay = abs(px - cx), abs(py - cy)
    qx, qy = ax - (hw - r), ay - (hh - r)
    sx = 1.0 if px >= cx else -1.0
    sy = 1.0 if py >= cy else -1.0
    if qx > 0 and qy > 0:
        l = math.hypot(qx, qy)
        return l - r, (sx * qx / l, sy * qy / l)
    if qx > qy:
        return qx - r, (sx, 0.0)
    return qy - r, (0.0, sy)


def body():
    """The handset as the original iPhone is built, drawn exactly rather than
    painted: every pixel is worked out from the shapes' own distances, with
    its coverage for smooth edges, so there is nothing to alias and no
    painted light. A polished steel bezel rolls round the edge; inside it a
    single flat pane of black glass covers the whole front, the display a
    hair beneath it; the earpiece slot and the iFruit mark above; the home
    button a shallow dish below. Three maps: the colour (body), the shape
    (phone_normal, x right and y up, z filled in by the shader) and what each
    part is made of (phone_material: R gloss, G smoothness, B metal, A how
    much it mirrors)."""
    TW, TH = BODY_TEX
    k = K  # pixels per phone unit

    def ux(v):
        return (v + SIDE) * k

    def u(v):
        return v * k

    colour = Image.new("RGBA", (TW, TH), (0, 0, 0, 0))
    normal = Image.new("RGBA", (TW, TH), (128, 128, 255, 255))
    material = Image.new("RGBA", (TW, TH), (0, 0, 0, 0))
    cp, np_, mp = colour.load(), normal.load(), material.load()

    # The body.
    ocx, ocy = ux(PU_W / 2), u(TOP + BODY_H / 2)
    ohw, ohh, orad = u(PU_W / 2), u(BODY_H / 2), u(32)
    bezel = u(6.5)        # the steel, from the outer edge in
    roll = u(5.2)         # how far in it has rolled flat
    seam = u(0.7)         # where the glass meets the steel
    STEEL = (188, 191, 197)
    STEEL_M = (215, 120, 255, 255)
    GLASS = (10, 10, 12)
    GLASS_M = (255, 245, 0, 255)
    # The display, under the glass.
    s = (ux(SCREEN[0]), u(SCREEN[1]), ux(SCREEN[2]), u(SCREEN[3]))
    # The home button.
    hx, hy, hr = ux(HOME[0]), u(HOME[1]), u(HOME[2])
    # The earpiece slot.
    ecx, ecy = (ux(EAR[0]) + ux(EAR[2])) / 2, (u(EAR[1]) + u(EAR[3])) / 2
    ehw, ehh = (ux(EAR[2]) - ux(EAR[0])) / 2, (u(EAR[3]) - u(EAR[1])) / 2
    # The side buttons: capsules standing out of the edge.
    buttons = []
    for top, bottom, right in ((RING[0], RING[1], False), (VOL_UP[0], VOL_UP[1], False),
                               (VOL_DOWN[0], VOL_DOWN[1], False), (SLEEP[0], SLEEP[1], True)):
        out = u(BUTTON_OUT)
        x0 = ux(PU_W) - u(6) if right else ux(0) - out
        x1 = ux(PU_W) + out if right else ux(0) + u(6)
        buttons.append(((x0 + x1) / 2, (u(top) + u(bottom)) / 2, (x1 - x0) / 2, (u(bottom) - u(top)) / 2, u(3)))

    def cover(d):
        return max(0.0, min(1.0, 0.5 - d))

    def tilt(dx, dy, a):
        """A normal leaning `a` radians toward (dx, dy), given with y down."""
        sa = math.sin(a)
        return dx * sa, -dy * sa

    def put(x, y, rgb, alpha, n, m):
        cp[x, y] = (int(rgb[0]), int(rgb[1]), int(rgb[2]), int(round(255 * alpha)))
        np_[x, y] = (int(round(128 + 127 * n[0])), int(round(128 + 127 * n[1])), 255, 255)
        mp[x, y] = m

    x_lo = int(ux(0) - u(BUTTON_OUT) - 2)
    x_hi = int(ux(PU_W) + u(BUTTON_OUT) + 2)
    for y in range(TH):
        py = y + 0.5
        for x in range(max(0, x_lo), min(TW, x_hi + 1)):
            px = x + 0.5
            d, (gx, gy) = rounded_box(px, py, ocx, ocy, ohw, ohh, orad)
            a_body = cover(d)
            # Behind the body: a side button, where there is one.
            under = None
            if a_body < 1.0:
                for bcx, bcy, bhw, bhh, br in buttons:
                    bd, (bx, by) = rounded_box(px, py, bcx, bcy, bhw, bhh, br)
                    if bd < 0.5:
                        t = min(1.0, -bd / u(2.4)) if bd < 0 else 0.0
                        ang = 1.25 * (1 - t) ** 1.6
                        shade = 0.82 + 0.18 * t
                        under = ((STEEL[0] * shade, STEEL[1] * shade, STEEL[2] * shade), cover(bd),
                                 tilt(bx, by, ang), STEEL_M)
                        break
            if a_body <= 0.0:
                if under:
                    put(x, y, under[0], under[1], under[2], under[3])
                continue
            inside = -d
            if inside < bezel:
                # The steel, rolling off round the edge: steep at the rim,
                # flat where it meets the glass. A soft dark line at the
                # very rim keeps the silhouette.
                t = min(1.0, inside / roll)
                ang = 1.35 * (1 - t) ** 1.8
                n = tilt(gx, gy, ang)
                rim = min(1.0, inside / u(1.2))
                shade = 0.35 + 0.65 * rim
                rgb = (STEEL[0] * shade, STEEL[1] * shade, STEEL[2] * shade)
                # The seam where the glass sits in the steel.
                sd = abs(inside - (bezel - seam * 0.5))
                if sd < seam:
                    f = 1 - sd / seam
                    rgb = tuple(c * (1 - 0.7 * f) for c in rgb)
                m = STEEL_M
            else:
                # The glass: flat, with its own edge ground a little round.
                g_in = inside - bezel
                n = tilt(gx, gy, 0.35 * max(0.0, 1 - g_in / u(1.4)) ** 2)
                rgb = GLASS
                m = GLASS_M
                # The display under it is drawn by the phone; here it is black.
                if s[0] <= px <= s[2] and s[1] <= py <= s[3]:
                    rgb = (4, 4, 5)
                # The earpiece: a slot through the glass, a fine grille in it.
                ed, (ex, ey) = rounded_box(px, py, ecx, ecy, ehw, ehh, ehh)
                if ed < 0.5:
                    f = cover(ed)
                    lift = 0.5 + 0.5 * math.sin(px * 1.9) * math.sin(py * 1.9)
                    grille = (34 + 10 * lift, 34 + 10 * lift, 38 + 10 * lift)
                    rgb = tuple(GLASS[i] * (1 - f) + grille[i] * f for i in range(3))
                    edge = max(0.0, 1 - abs(ed) / u(0.8))
                    n = tilt(-ex, -ey, 0.6 * edge)
                    m = (60, 60, 0, 60) if f > 0.5 else m
                # The home button: a hole in the glass, a shallow dish in it.
                hd = math.hypot(px - hx, py - hy)
                if hd < hr + 0.5:
                    f = cover(hd - hr)
                    ring_w = u(1.6)
                    if hd > hr - ring_w:
                        # The glass's edge dropping into the hole.
                        e = (hd - (hr - ring_w)) / ring_w
                        n = tilt(-(px - hx) / max(hd, 1e-3), -(py - hy) / max(hd, 1e-3), 0.9 * e)
                        rgb = tuple(GLASS[i] * (1 - f) + 16 * f for i in range(3))
                        m = GLASS_M if f < 0.5 else (230, 200, 0, 255)
                    else:
                        # The dish: concave, its sides facing its middle.
                        r = hd / (hr - ring_w)
                        n = tilt(-(px - hx) / max(hd, 1e-3), -(py - hy) / max(hd, 1e-3), 0.28 * r)
                        rgb = (30, 30, 34)
                        m = (190, 160, 0, 200)
                        # The rounded square on it.
                        q = hr * 0.34
                        sq, _ = rounded_box(px, py, hx, hy, q, q, q * 0.3)
                        line = cover(abs(sq) - u(0.8))
                        if line > 0:
                            rgb = tuple(rgb[i] * (1 - line) + 205 * line for i in range(3))
            a = a_body
            if under and a < 1.0:
                # The body's edge over the button behind it.
                rgb = tuple(rgb[i] * a + under[0][i] * (1 - a) for i in range(3))
                n = tuple(n[i] * a + under[2][i] * (1 - a) for i in range(2))
                a = a + under[1] * (1 - a)
                m = STEEL_M
            put(x, y, rgb, a, n, m)

    # The iFruit mark, in bright silver on the glass.
    mark = ifruit_mark()
    ms = int(u(24))
    mark = mark.resize((ms, ms), Image.LANCZOS)
    mx, my = int(ux(PU_W / 2) - ms / 2), int(u(TOP + 12))
    silver = Image.new("RGBA", (ms, ms), (200, 202, 208, 255))
    silver.putalpha(mark)
    colour.alpha_composite(silver, (mx, my))
    mat_mark = Image.new("RGBA", (ms, ms), (230, 160, 255, 255))
    material.paste(mat_mark, (mx, my), mark)

    colour.save(os.path.join(OUT, "body.png"))
    normal.save(os.path.join(OUT, "phone_normal.png"))
    material.save(os.path.join(OUT, "phone_material.png"))


# ---------------------------------------------------------------------------
# Icons, in the radar-icon style
# ---------------------------------------------------------------------------
ICON = 128
IS = ICON * SS


RADAR = 64  # the game's radar icons are 64 x 64


def radar_icon(shape, fill, name, extra=None):
    """Turn a white-on-transparent shape into an icon drawn the way the game's
    radar icons are (models/hud.txd): 64 pixels square with hard, stepped
    edges; flat saturated colour shaded in a few hard bands, light from the
    top left; a darker rim just inside the edge; a thick black outline; a
    hard white glint; and a little grain, as their compression leaves."""
    import random
    rng = random.Random(name)
    app = name.startswith('app_')
    pixels = 32 if app else RADAR
    mask = shape.getchannel("A").point(lambda v: 255 if v > 100 else 0)
    base = fill[:3]
    light = tuple(min(255, int(c * 1.25 + 40)) for c in base)
    dark = tuple(int(c * 0.55) for c in base)
    # Three hard bands across the diagonal.
    bands = Image.new("RGBA", (IS, IS))
    px = bands.load()
    for y in range(IS):
        for x in range(0, IS):
            t = (x * 0.4 + y) / (IS * 1.4)
            # Five steps from light to dark.
            step = min(4, int(t * 5)) / 4.0
            px[x, y] = (lerp(light, base, step * 2) if step <= 0.5 else lerp(base, dark, step * 2 - 1)) + (255,)
    img = Image.new("RGBA", (IS, IS), (0, 0, 0, 0))
    img.alpha_composite(fill_mask(mask, bands))
    if extra:
        extra(img, mask)
    # Filling the square, as the game's do, leaving room for the edge.
    bb = mask.getbbox()
    if bb:
        crop = img.crop(bb)
        f = IS * (0.70 if app else 0.8) / max(crop.size)
        crop = crop.resize((max(1, int(crop.width * f)), max(1, int(crop.height * f))), Image.LANCZOS)
        img = Image.new("RGBA", (IS, IS), (0, 0, 0, 0))
        img.alpha_composite(crop, ((IS - crop.width) // 2, (IS - crop.height) // 2))
    small = img.resize((pixels, pixels), Image.BOX)
    hard = small.getchannel("A").point(lambda v: 255 if v > 110 else 0)
    # A darker rim one pixel inside the edge.
    inner = hard.filter(ImageFilter.MinFilter(3))
    rim = ImageChops.subtract(hard, inner)
    rim_colour = tuple(int(c * 0.4) for c in base) + (255,)
    out = Image.new("RGBA", (pixels, pixels), (0, 0, 0, 0))
    # App outlines are six pixels wide at the final size; other glyphs retain four.
    out.alpha_composite(fill_mask(hard.filter(ImageFilter.MaxFilter(7 if app else 9)), BLACK))
    body = small.copy()
    body.putalpha(hard)
    out.alpha_composite(body)
    out.alpha_composite(fill_mask(rim, rim_colour))
    # The glint: a short hard white stroke near the top left of the shape.
    bb = inner.getbbox()
    if bb:
        gx, gy = bb[0] + (bb[2] - bb[0]) * 0.22, bb[1] + (bb[3] - bb[1]) * 0.18
        glint = Image.new("L", (pixels, pixels), 0)
        extent = 1.5 if app else 3
        ImageDraw.Draw(glint).ellipse((gx - extent, gy - extent * 0.5, gx + extent, gy + extent * 0.5), fill=255)
        glint = ImageChops.multiply(glint, inner)
        out.alpha_composite(fill_mask(glint, (255, 255, 255, 230)))
    # Grain.
    opx = out.load()
    for y in range(pixels):
        for x in range(pixels):
            r, g, b, a = opx[x, y]
            if a and (r, g, b) != (0, 0, 0):
                n = rng.randint(-9, 9)
                opx[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)), max(0, min(255, b + n)), a)
    if app:
        # Keep the coarse two-pixel steps of the stock spanner icon.
        out = out.resize((RADAR, RADAR), Image.Resampling.NEAREST)
    out.save(os.path.join(OUT, name + ".png"))


def canvas():
    return Image.new("RGBA", (IS, IS), (0, 0, 0, 0))


def handset_shape():
    """A quarter ring bowing to the top left, deeper at both ends where the
    ear and mouth cups are."""
    S = IS
    cx, cy = S * 0.80, S * 0.80
    R = S * 0.56
    t = S * 0.12
    m = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(m)
    d.pieslice((cx - R, cy - R, cx + R, cy + R), 180, 270, fill=255)
    ri = R - t
    d.pieslice((cx - ri, cy - ri, cx + ri, cy + ri), 180, 270, fill=0)
    for a0, a1 in ((180, 204), (246, 270)):
        cup = Image.new("L", (S, S), 0)
        cd = ImageDraw.Draw(cup)
        cd.pieslice((cx - R, cy - R, cx + R, cy + R), a0, a1, fill=255)
        rc = R - t * 2.2
        cd.pieslice((cx - rc, cy - rc, cx + rc, cy + rc), a0 - 1, a1 + 1, fill=0)
        m = ImageChops.lighter(m, cup)
    m = m.filter(ImageFilter.GaussianBlur(S * 0.02)).point(lambda v: 255 if v > 128 else 0)
    bb = m.getbbox()
    out = Image.new("L", (S, S), 0)
    out.paste(m.crop(bb), ((S - (bb[2] - bb[0])) // 2, (S - (bb[3] - bb[1])) // 2))
    shape = canvas()
    shape.putalpha(out)
    return shape


def bubble_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.ellipse((IS * 0.12, IS * 0.16, IS * 0.88, IS * 0.70), fill=(255, 255, 255, 255))
    d.polygon([(IS * 0.28, IS * 0.58), (IS * 0.18, IS * 0.86), (IS * 0.48, IS * 0.66)],
              fill=(255, 255, 255, 255))
    return s


def person_shape():
    """SP-RP's contact glyph: the person, without the plus beside it."""
    g = Image.open(os.path.join(ASSETS, "sprp", "phone_contact.png")).convert("RGBA")
    a = g.getchannel("A").point(lambda v: 255 if v > 100 else 0)
    g = g.crop(a.getbbox())
    g = g.crop((0, 0, int(g.width * 0.55), g.height))
    f = IS * 0.72 / max(g.size)
    g = g.resize((int(g.width * f), int(g.height * f)), Image.LANCZOS)
    s = canvas()
    s.alpha_composite(g, ((IS - g.width) // 2, (IS - g.height) // 2))
    white = Image.new("RGBA", s.size, (255, 255, 255, 255))
    white.putalpha(s.getchannel("A").point(lambda v: 255 if v > 100 else 0))
    return white


def disc_shape():
    s = canvas()
    ImageDraw.Draw(s).ellipse((IS * 0.12, IS * 0.12, IS * 0.88, IS * 0.88), fill=(255, 255, 255, 255))
    return s


def clock_extra(img, mask):
    d = ImageDraw.Draw(img)
    c = IS / 2
    fr = IS * 0.38
    for i in range(12):
        a = math.radians(i * 30)
        l0 = fr * (0.72 if i % 3 == 0 else 0.82)
        d.line((c + math.cos(a) * l0, c + math.sin(a) * l0, c + math.cos(a) * fr * 0.93,
                c + math.sin(a) * fr * 0.93), fill=BLACK, width=int(IS * (0.03 if i % 3 == 0 else 0.015)))
    d.line((c, c, c + fr * 0.5, c - fr * 0.18), fill=BLACK, width=int(IS * 0.05))
    d.line((c, c, c - fr * 0.08, c - fr * 0.76), fill=BLACK, width=int(IS * 0.035))
    d.ellipse((c - IS * 0.035, c - IS * 0.035, c + IS * 0.035, c + IS * 0.035), fill=BLACK)


def globe_extra(img, mask):
    d = ImageDraw.Draw(img)
    c = IS / 2
    r = IS * 0.38
    w = int(IS * 0.028)
    d.ellipse((c - r * 0.45, c - r, c + r * 0.45, c + r), outline=BLACK, width=w)
    d.line((c, c - r, c, c + r), fill=BLACK, width=w)
    d.line((c - r, c, c + r, c), fill=BLACK, width=w)
    for f in (-0.55, 0.55):
        half = r * math.sqrt(1 - f * f)
        d.line((c - half, c + f * r, c + half, c + f * r), fill=BLACK, width=w)


def gear_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    c = IS / 2
    for i in range(8):
        a = math.radians(i * 45)
        d.polygon([(c + math.cos(a - 0.2) * IS * 0.28, c + math.sin(a - 0.2) * IS * 0.28),
                   (c + math.cos(a - 0.13) * IS * 0.40, c + math.sin(a - 0.13) * IS * 0.40),
                   (c + math.cos(a + 0.13) * IS * 0.40, c + math.sin(a + 0.13) * IS * 0.40),
                   (c + math.cos(a + 0.2) * IS * 0.28, c + math.sin(a + 0.2) * IS * 0.28)],
                  fill=(255, 255, 255, 255))
    d.ellipse((c - IS * 0.3, c - IS * 0.3, c + IS * 0.3, c + IS * 0.3), fill=(255, 255, 255, 255))
    d.ellipse((c - IS * 0.1, c - IS * 0.1, c + IS * 0.1, c + IS * 0.1), fill=(0, 0, 0, 0))
    return s


def camera_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.rounded_rectangle((IS * 0.14, IS * 0.3, IS * 0.86, IS * 0.78), radius=IS * 0.06, fill=(255, 255, 255, 255))
    d.rectangle((IS * 0.36, IS * 0.2, IS * 0.62, IS * 0.32), fill=(255, 255, 255, 255))
    return s


def camera_extra(img, mask):
    ImageDraw.Draw(img).ellipse((IS * 0.36, IS * 0.38, IS * 0.64, IS * 0.66), fill=(40, 40, 44, 255),
                                outline=BLACK, width=int(IS * 0.03))


def joystick_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.rounded_rectangle((IS * 0.14, IS * 0.62, IS * 0.86, IS * 0.86), radius=IS * 0.08, fill=(255, 255, 255, 255))
    d.rectangle((IS * 0.46, IS * 0.30, IS * 0.54, IS * 0.66), fill=(255, 255, 255, 255))
    d.ellipse((IS * 0.33, IS * 0.10, IS * 0.67, IS * 0.44), fill=(255, 255, 255, 255))
    return s


def joystick_extra(img, mask):
    d = ImageDraw.Draw(img)
    # A red ball on a black stick, on a grey base with one red button.
    d.rectangle((IS * 0.46, IS * 0.30, IS * 0.54, IS * 0.64), fill=(30, 30, 32, 255))
    d.ellipse((IS * 0.33, IS * 0.10, IS * 0.67, IS * 0.44), fill=(215, 40, 40, 255), outline=BLACK,
              width=int(IS * 0.03))
    d.ellipse((IS * 0.66, IS * 0.66, IS * 0.78, IS * 0.78), fill=(215, 40, 40, 255), outline=BLACK,
              width=int(IS * 0.02))


# The Games app's machines, one icon each.

def duality_extra(img, mask):
    d = ImageDraw.Draw(img)
    # Dark on the left, light on the right, and the ship on the line between.
    box = (IS * 0.12, IS * 0.12, IS * 0.88, IS * 0.88)
    d.pieslice(box, 90, 270, fill=(34, 34, 40, 255))
    d.pieslice(box, 270, 90, fill=(240, 240, 236, 255))
    c = IS / 2
    d.polygon([(c, IS * 0.30), (c + IS * 0.12, IS * 0.66), (c, IS * 0.58), (c - IS * 0.12, IS * 0.66)],
              fill=(215, 40, 40, 255), outline=BLACK)


def bee_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.ellipse((IS * 0.14, IS * 0.36, IS * 0.86, IS * 0.84), fill=(255, 255, 255, 255))
    d.ellipse((IS * 0.20, IS * 0.10, IS * 0.50, IS * 0.46), fill=(255, 255, 255, 255))
    d.ellipse((IS * 0.50, IS * 0.10, IS * 0.80, IS * 0.46), fill=(255, 255, 255, 255))
    return s


def bee_extra(img, mask):
    d = ImageDraw.Draw(img)
    # Pale wings over a yellow body with black bands and a black head.
    for x0 in (0.20, 0.50):
        d.ellipse((IS * x0, IS * 0.10, IS * (x0 + 0.30), IS * 0.46), fill=(210, 235, 250, 255), outline=BLACK,
                  width=int(IS * 0.02))
    body = Image.new("L", (IS, IS), 0)
    ImageDraw.Draw(body).ellipse((IS * 0.14, IS * 0.36, IS * 0.86, IS * 0.84), fill=255)
    bands = Image.new("L", (IS, IS), 0)
    bd = ImageDraw.Draw(bands)
    for x0 in (0.36, 0.56):
        bd.rectangle((IS * x0, 0, IS * (x0 + 0.10), IS), fill=255)
    bd.rectangle((IS * 0.72, 0, IS, IS), fill=255)
    img.alpha_composite(fill_mask(ImageChops.multiply(body, bands), (28, 26, 22, 255)))


def planet_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.ellipse((IS * 0.24, IS * 0.24, IS * 0.76, IS * 0.76), fill=(255, 255, 255, 255))
    d.ellipse((IS * 0.04, IS * 0.40, IS * 0.96, IS * 0.62), fill=(255, 255, 255, 255))
    return s


def planet_extra(img, mask):
    d = ImageDraw.Draw(img)
    # The ring in front of the planet's lower half, with the gap behind it.
    d.ellipse((IS * 0.04, IS * 0.40, IS * 0.96, IS * 0.62), fill=(230, 200, 120, 255), outline=BLACK,
              width=int(IS * 0.02))
    d.ellipse((IS * 0.18, IS * 0.46, IS * 0.82, IS * 0.56), fill=(60, 120, 140, 255))
    d.chord((IS * 0.24, IS * 0.24, IS * 0.76, IS * 0.76), 180, 360, fill=(110, 200, 210, 255))
    d.line((IS * 0.25, IS * 0.36, IS * 0.75, IS * 0.36), fill=(80, 170, 185, 255), width=int(IS * 0.03))


def saucer_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.ellipse((IS * 0.30, IS * 0.18, IS * 0.70, IS * 0.58), fill=(255, 255, 255, 255))
    d.ellipse((IS * 0.06, IS * 0.42, IS * 0.94, IS * 0.74), fill=(255, 255, 255, 255))
    return s


def saucer_extra(img, mask):
    d = ImageDraw.Draw(img)
    # A green glass dome on a grey saucer with a row of yellow lights.
    d.chord((IS * 0.30, IS * 0.18, IS * 0.70, IS * 0.58), 180, 360, fill=(120, 220, 120, 255), outline=BLACK,
            width=int(IS * 0.02))
    for i in range(5):
        x = IS * (0.22 + i * 0.14)
        d.ellipse((x - IS * 0.04, IS * 0.54, x + IS * 0.04, IS * 0.62), fill=(250, 220, 60, 255))


def game_icons():
    radar_icon(disc_shape(), (140, 140, 146, 255), "game_duality", duality_extra)
    radar_icon(bee_shape(), (245, 200, 40, 255), "game_bumble", bee_extra)
    radar_icon(planet_shape(), (110, 200, 210, 255), "game_uranus", planet_extra)
    radar_icon(saucer_shape(), (170, 172, 180, 255), "game_spacemonkey", saucer_extra)


def calculator_shape():
    s = canvas()
    ImageDraw.Draw(s).rounded_rectangle((IS * 0.2, IS * 0.1, IS * 0.8, IS * 0.9), radius=IS * 0.07,
                                        fill=(255, 255, 255, 255))
    return s


def calculator_extra(img, mask):
    d = ImageDraw.Draw(img)
    # The display, and three rows of three keys under it.
    d.rectangle((IS * 0.28, IS * 0.18, IS * 0.72, IS * 0.34), fill=(150, 190, 140, 255), outline=BLACK,
                width=int(IS * 0.02))
    for r in range(3):
        for c in range(3):
            x, y = IS * (0.28 + c * 0.155), IS * (0.42 + r * 0.14)
            d.rectangle((x, y, x + IS * 0.13, y + IS * 0.1), fill=(40, 40, 44, 255))


def notes_shape():
    s = canvas()
    ImageDraw.Draw(s).rectangle((IS * 0.18, IS * 0.12, IS * 0.82, IS * 0.88), fill=(255, 255, 255, 255))
    return s


def notes_extra(img, mask):
    d = ImageDraw.Draw(img)
    # A yellow legal pad: a brown binding along the top, and ruled lines.
    d.rectangle((IS * 0.18, IS * 0.12, IS * 0.82, IS * 0.24), fill=(120, 80, 40, 255))
    for i in range(5):
        y = IS * (0.36 + i * 0.1)
        d.line((IS * 0.24, y, IS * 0.76, y), fill=(90, 120, 170, 255), width=int(IS * 0.015))


def map_shape():
    # A map folded in three, the panels leaning in and out.
    s = canvas()
    ImageDraw.Draw(s).polygon([(IS * 0.1, IS * 0.2), (IS * 0.37, IS * 0.12), (IS * 0.63, IS * 0.2),
                               (IS * 0.9, IS * 0.12), (IS * 0.9, IS * 0.8), (IS * 0.63, IS * 0.88),
                               (IS * 0.37, IS * 0.8), (IS * 0.1, IS * 0.88)], fill=(255, 255, 255, 255))
    return s


def map_extra(img, mask):
    d = ImageDraw.Draw(img)
    # Land, a river across it, the folds, and a red pin.
    d.polygon([(IS * 0.1, IS * 0.55), (IS * 0.9, IS * 0.42), (IS * 0.9, IS * 0.52), (IS * 0.1, IS * 0.66)],
              fill=(80, 140, 220, 255))
    for x, shade in ((0.37, (0, 0, 0, 70)), (0.63, (255, 255, 255, 60))):
        d.line((IS * x, IS * 0.12, IS * x, IS * 0.88), fill=shade, width=int(IS * 0.02))
    d.ellipse((IS * 0.48, IS * 0.22, IS * 0.7, IS * 0.44), fill=(220, 40, 40, 255))
    d.polygon([(IS * 0.5, IS * 0.36), (IS * 0.68, IS * 0.36), (IS * 0.59, IS * 0.58)], fill=(220, 40, 40, 255))
    d.ellipse((IS * 0.555, IS * 0.29, IS * 0.625, IS * 0.36), fill=(255, 255, 255, 255))


def g_power():
    # The power mark: a ring broken at the top, and a stroke through the gap.
    s = canvas()
    d = ImageDraw.Draw(s)
    w = int(IS * 0.12)
    d.arc((IS * 0.18, IS * 0.2, IS * 0.82, IS * 0.84), start=-50, end=230, fill=WHITE, width=w)
    d.rounded_rectangle((IS * 0.44, IS * 0.08, IS * 0.56, IS * 0.5), radius=IS * 0.05, fill=WHITE)
    return s


def plus_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.rectangle((IS * 0.40, IS * 0.14, IS * 0.60, IS * 0.86), fill=(255, 255, 255, 255))
    d.rectangle((IS * 0.14, IS * 0.40, IS * 0.86, IS * 0.60), fill=(255, 255, 255, 255))
    return s


def back_shape():
    """GTA V's back key: an arrow bent back on itself."""
    s = canvas()
    d = ImageDraw.Draw(s)
    w = int(IS * 0.16)
    d.arc((IS * 0.28, IS * 0.18, IS * 0.86, IS * 0.76), start=270, end=90, fill=(255, 255, 255, 255), width=w)
    d.line((IS * 0.30, IS * 0.26, IS * 0.58, IS * 0.26), fill=(255, 255, 255, 255), width=w)
    d.line((IS * 0.30, IS * 0.68, IS * 0.58, IS * 0.68), fill=(255, 255, 255, 255), width=w)
    d.polygon([(IS * 0.08, IS * 0.68), (IS * 0.34, IS * 0.48), (IS * 0.34, IS * 0.88)], fill=(255, 255, 255, 255))
    return s



# ---------------------------------------------------------------------------
# Glyphs: everything inside the apps that a button or a row needs a picture
# for, drawn the same way as the app icons.
# ---------------------------------------------------------------------------
WHITE = (255, 255, 255, 255)


def g_backspace():
    s = canvas()
    ImageDraw.Draw(s).polygon([(IS * 0.06, IS * 0.5), (IS * 0.34, IS * 0.2), (IS * 0.94, IS * 0.2),
                               (IS * 0.94, IS * 0.8), (IS * 0.34, IS * 0.8)], fill=WHITE)
    return s


def x_backspace(img, mask):
    d = ImageDraw.Draw(img)
    w = int(IS * 0.07)
    d.line((IS * 0.46, IS * 0.36, IS * 0.74, IS * 0.64), fill=BLACK, width=w)
    d.line((IS * 0.46, IS * 0.64, IS * 0.74, IS * 0.36), fill=BLACK, width=w)


def g_person_plus():
    s = person_shape()
    d = ImageDraw.Draw(s)
    d.rectangle((IS * 0.70, IS * 0.10, IS * 0.80, IS * 0.40), fill=WHITE)
    d.rectangle((IS * 0.60, IS * 0.20, IS * 0.90, IS * 0.30), fill=WHITE)
    return s


def g_star():
    s = canvas()
    pts = []
    for i in range(10):
        r = IS * (0.46 if i % 2 == 0 else 0.2)
        a = math.radians(-90 + i * 36)
        pts.append((IS / 2 + math.cos(a) * r, IS * 0.53 + math.sin(a) * r))
    ImageDraw.Draw(s).polygon(pts, fill=WHITE)
    return s


def g_dots():
    s = canvas()
    d = ImageDraw.Draw(s)
    for r in range(4):
        for c in range(3):
            if r == 3 and c != 1:
                continue
            x, y = IS * (0.27 + c * 0.23), IS * (0.14 + r * 0.24)
            d.ellipse((x - IS * 0.085, y - IS * 0.085, x + IS * 0.085, y + IS * 0.085), fill=WHITE)
    return s


def g_mic():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.rounded_rectangle((IS * 0.36, IS * 0.08, IS * 0.64, IS * 0.58), radius=IS * 0.14, fill=WHITE)
    d.arc((IS * 0.24, IS * 0.24, IS * 0.76, IS * 0.72), 0, 180, fill=WHITE, width=int(IS * 0.07))
    d.rectangle((IS * 0.46, IS * 0.70, IS * 0.54, IS * 0.86), fill=WHITE)
    d.rectangle((IS * 0.3, IS * 0.84, IS * 0.7, IS * 0.92), fill=WHITE)
    return s


def x_slash(img, mask):
    ImageDraw.Draw(img).line((IS * 0.18, IS * 0.1, IS * 0.82, IS * 0.9), fill=(200, 30, 30, 255), width=int(IS * 0.08))


def g_speaker():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.polygon([(IS * 0.08, IS * 0.36), (IS * 0.26, IS * 0.36), (IS * 0.5, IS * 0.14), (IS * 0.5, IS * 0.86),
               (IS * 0.26, IS * 0.64), (IS * 0.08, IS * 0.64)], fill=WHITE)
    for r in (0.2, 0.34):
        d.arc((IS * (0.5 - r), IS * (0.5 - r), IS * (0.5 + r), IS * (0.5 + r)), -50, 50, fill=WHITE,
              width=int(IS * 0.07))
    return s


def g_pause():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.rectangle((IS * 0.24, IS * 0.16, IS * 0.42, IS * 0.84), fill=WHITE)
    d.rectangle((IS * 0.58, IS * 0.16, IS * 0.76, IS * 0.84), fill=WHITE)
    return s


def g_book():
    s = canvas()
    ImageDraw.Draw(s).rounded_rectangle((IS * 0.2, IS * 0.1, IS * 0.8, IS * 0.9), radius=IS * 0.05, fill=WHITE)
    return s


def x_book(img, mask):
    d = ImageDraw.Draw(img)
    d.rectangle((IS * 0.2, IS * 0.1, IS * 0.3, IS * 0.9), fill=(60, 40, 20, 255))
    for i in range(3):
        d.rectangle((IS * 0.24, IS * (0.24 + i * 0.22), IS * 0.34, IS * (0.3 + i * 0.22)), fill=(220, 220, 220, 255))


def g_pencil():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.polygon([(IS * 0.14, IS * 0.86), (IS * 0.2, IS * 0.64), (IS * 0.66, IS * 0.18), (IS * 0.82, IS * 0.34),
               (IS * 0.36, IS * 0.8)], fill=WHITE)
    return s


def x_pencil(img, mask):
    d = ImageDraw.Draw(img)
    d.polygon([(IS * 0.14, IS * 0.86), (IS * 0.2, IS * 0.64), (IS * 0.36, IS * 0.8)], fill=(240, 210, 160, 255))
    d.polygon([(IS * 0.6, IS * 0.24), (IS * 0.66, IS * 0.18), (IS * 0.82, IS * 0.34), (IS * 0.76, IS * 0.4)],
              fill=(230, 120, 140, 255))


def g_bin():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.polygon([(IS * 0.22, IS * 0.3), (IS * 0.78, IS * 0.3), (IS * 0.7, IS * 0.9), (IS * 0.3, IS * 0.9)], fill=WHITE)
    d.rectangle((IS * 0.14, IS * 0.18, IS * 0.86, IS * 0.26), fill=WHITE)
    d.rectangle((IS * 0.4, IS * 0.1, IS * 0.6, IS * 0.18), fill=WHITE)
    return s


def x_bin(img, mask):
    d = ImageDraw.Draw(img)
    for x in (0.38, 0.5, 0.62):
        d.line((IS * x, IS * 0.4, IS * x, IS * 0.8), fill=BLACK, width=int(IS * 0.04))


def g_house():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.polygon([(IS * 0.5, IS * 0.1), (IS * 0.92, IS * 0.5), (IS * 0.08, IS * 0.5)], fill=WHITE)
    d.rectangle((IS * 0.2, IS * 0.46, IS * 0.8, IS * 0.9), fill=WHITE)
    return s


def x_house(img, mask):
    ImageDraw.Draw(img).rectangle((IS * 0.42, IS * 0.62, IS * 0.58, IS * 0.9), fill=(90, 50, 30, 255))


def g_arrow(right):
    s = canvas()
    d = ImageDraw.Draw(s)
    if right:
        d.polygon([(IS * 0.14, IS * 0.36), (IS * 0.5, IS * 0.36), (IS * 0.5, IS * 0.12), (IS * 0.9, IS * 0.5),
                   (IS * 0.5, IS * 0.88), (IS * 0.5, IS * 0.64), (IS * 0.14, IS * 0.64)], fill=WHITE)
    else:
        d.polygon([(IS * 0.86, IS * 0.36), (IS * 0.5, IS * 0.36), (IS * 0.5, IS * 0.12), (IS * 0.1, IS * 0.5),
                   (IS * 0.5, IS * 0.88), (IS * 0.5, IS * 0.64), (IS * 0.86, IS * 0.64)], fill=WHITE)
    return s


def g_bell():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.pieslice((IS * 0.18, IS * 0.12, IS * 0.82, IS * 0.9), 180, 360, fill=WHITE)
    d.rectangle((IS * 0.18, IS * 0.5, IS * 0.82, IS * 0.72), fill=WHITE)
    d.polygon([(IS * 0.18, IS * 0.72), (IS * 0.82, IS * 0.72), (IS * 0.92, IS * 0.8), (IS * 0.08, IS * 0.8)],
              fill=WHITE)
    d.ellipse((IS * 0.42, IS * 0.78, IS * 0.58, IS * 0.94), fill=WHITE)
    d.ellipse((IS * 0.44, IS * 0.04, IS * 0.56, IS * 0.16), fill=WHITE)
    return s


def g_stopwatch():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.ellipse((IS * 0.14, IS * 0.2, IS * 0.86, IS * 0.92), fill=WHITE)
    d.rectangle((IS * 0.42, IS * 0.06, IS * 0.58, IS * 0.2), fill=WHITE)
    return s


def x_stopwatch(img, mask):
    d = ImageDraw.Draw(img)
    c = (IS * 0.5, IS * 0.56)
    d.line((c[0], c[1], IS * 0.66, IS * 0.36), fill=(200, 30, 30, 255), width=int(IS * 0.05))
    d.ellipse((c[0] - IS * 0.04, c[1] - IS * 0.04, c[0] + IS * 0.04, c[1] + IS * 0.04), fill=BLACK)


def g_hourglass():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.rectangle((IS * 0.18, IS * 0.08, IS * 0.82, IS * 0.16), fill=WHITE)
    d.rectangle((IS * 0.18, IS * 0.84, IS * 0.82, IS * 0.92), fill=WHITE)
    d.polygon([(IS * 0.24, IS * 0.16), (IS * 0.76, IS * 0.16), (IS * 0.54, IS * 0.5), (IS * 0.76, IS * 0.84),
               (IS * 0.24, IS * 0.84), (IS * 0.46, IS * 0.5)], fill=WHITE)
    return s


def x_hourglass(img, mask):
    d = ImageDraw.Draw(img)
    sand = (230, 190, 90, 255)
    d.polygon([(IS * 0.34, IS * 0.3), (IS * 0.66, IS * 0.3), (IS * 0.5, IS * 0.46)], fill=sand)
    d.polygon([(IS * 0.5, IS * 0.62), (IS * 0.7, IS * 0.82), (IS * 0.3, IS * 0.82)], fill=sand)


def g_picture():
    s = canvas()
    ImageDraw.Draw(s).rectangle((IS * 0.1, IS * 0.18, IS * 0.9, IS * 0.82), fill=WHITE)
    return s


def x_picture(img, mask):
    d = ImageDraw.Draw(img)
    d.rectangle((IS * 0.16, IS * 0.24, IS * 0.84, IS * 0.76), fill=(120, 180, 230, 255))
    d.polygon([(IS * 0.16, IS * 0.76), (IS * 0.4, IS * 0.44), (IS * 0.58, IS * 0.64), (IS * 0.68, IS * 0.54),
               (IS * 0.84, IS * 0.76)], fill=(70, 140, 60, 255))
    d.ellipse((IS * 0.64, IS * 0.3, IS * 0.76, IS * 0.42), fill=(250, 220, 80, 255))


def g_padlock():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.arc((IS * 0.28, IS * 0.08, IS * 0.72, IS * 0.6), 180, 360, fill=WHITE, width=int(IS * 0.1))
    d.rectangle((IS * 0.28, IS * 0.32, IS * 0.36, IS * 0.46), fill=WHITE)
    d.rectangle((IS * 0.64, IS * 0.32, IS * 0.72, IS * 0.46), fill=WHITE)
    d.rounded_rectangle((IS * 0.16, IS * 0.44, IS * 0.84, IS * 0.92), radius=IS * 0.06, fill=WHITE)
    return s


def g_info():
    return disc_shape()


def x_info(img, mask):
    d = ImageDraw.Draw(img)
    d.ellipse((IS * 0.45, IS * 0.24, IS * 0.55, IS * 0.34), fill=WHITE)
    d.rectangle((IS * 0.45, IS * 0.4, IS * 0.55, IS * 0.74), fill=WHITE)


def g_magnifier():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.ellipse((IS * 0.1, IS * 0.1, IS * 0.64, IS * 0.64), outline=WHITE, width=int(IS * 0.11))
    d.line((IS * 0.56, IS * 0.56, IS * 0.88, IS * 0.88), fill=WHITE, width=int(IS * 0.14))
    return s


def g_up():
    s = canvas()
    ImageDraw.Draw(s).polygon([(IS * 0.5, IS * 0.08), (IS * 0.9, IS * 0.5), (IS * 0.64, IS * 0.5),
                               (IS * 0.64, IS * 0.9), (IS * 0.36, IS * 0.9), (IS * 0.36, IS * 0.5),
                               (IS * 0.1, IS * 0.5)], fill=WHITE)
    return s


def g_shutter():
    return disc_shape()


def x_shutter(img, mask):
    d = ImageDraw.Draw(img)
    d.ellipse((IS * 0.3, IS * 0.3, IS * 0.7, IS * 0.7), fill=(40, 40, 44, 255), outline=BLACK, width=int(IS * 0.03))


def x_flip(img, mask):
    """Two arrows chasing each other round the lens: switch cameras."""
    d = ImageDraw.Draw(img)
    c, r, w = IS * 0.5, IS * 0.16, int(IS * 0.045)
    box = (c - r, IS * 0.54 - r, c + r, IS * 0.54 + r)
    d.arc(box, 200, 340, fill=BLACK, width=w)
    d.arc(box, 20, 160, fill=BLACK, width=w)
    cy = IS * 0.54
    a = IS * 0.07
    # Arrowheads at the end of each arc.
    x1, y1 = c + r * 0.94, cy - r * 0.34
    d.polygon([(x1 - a, y1 - a * 0.2), (x1 + a, y1 - a * 0.2), (x1, y1 + a)], fill=BLACK)
    x2, y2 = c - r * 0.94, cy + r * 0.34
    d.polygon([(x2 - a, y2 + a * 0.2), (x2 + a, y2 + a * 0.2), (x2, y2 - a)], fill=BLACK)


def camera_controls():
    """The viewfinder's shutter button, a white disc in a white ring as the
    phone's camera has, and a plain disc for round buttons and thumbnails."""
    S = 128 * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((0, 0, S - 1, S - 1), fill=BLACK)
    e = S * 0.04
    d.ellipse((e, e, S - e, S - e), fill=(250, 250, 250, 255))
    g = S * 0.1
    d.ellipse((g, g, S - g, S - g), fill=BLACK)
    i = S * 0.13
    d.ellipse((i, i, S - i, S - i), fill=(250, 250, 250, 255))
    finish(img, (128, 128), "cam_shutter")

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(img).ellipse((0, 0, S - 1, S - 1), fill=(255, 255, 255, 255))
    finish(img, (128, 128), "disc")


def glyphs():
    """Each picture in its own colour, as the radar icons are."""
    green, red, grey, blue, yellow = (70, 190, 60, 255), (215, 40, 40, 255), (170, 172, 180, 255), \
        (80, 150, 230, 255), (240, 200, 60, 255)
    radar_icon(handset_shape(), green, "g_call")
    # Hung up: the handset turned to lie flat, earpiece to the left.
    radar_icon(handset_shape().rotate(135, resample=Image.BICUBIC), red, "g_end")
    radar_icon(g_backspace(), grey, "g_backspace", x_backspace)
    radar_icon(g_person_plus(), (240, 190, 70, 255), "g_addcontact")
    radar_icon(g_star(), yellow, "g_favorites")
    radar_icon(disc_shape(), (250, 250, 250, 255), "g_recents", clock_extra)
    radar_icon(person_shape(), (240, 190, 70, 255), "g_contacts")
    radar_icon(g_dots(), blue, "g_keypad")
    radar_icon(g_mic(), grey, "g_mute", x_slash)
    radar_icon(g_speaker(), blue, "g_speaker")
    radar_icon(plus_shape(), green, "g_add")
    radar_icon(g_pause(), yellow, "g_hold")
    radar_icon(g_book(), (190, 120, 60, 255), "g_book", x_book)
    radar_icon(g_pencil(), (250, 200, 60, 255), "g_compose", x_pencil)
    radar_icon(g_bin(), grey, "g_trash", x_bin)
    radar_icon(g_house(), (230, 90, 60, 255), "g_home", x_house)
    radar_icon(g_arrow(False), blue, "g_back")
    radar_icon(g_arrow(True), blue, "g_forward")
    radar_icon(g_bell(), yellow, "g_alarm")
    # The ring switch's picture in the middle of the screen (the silent one
    # is this, struck through).
    radar_icon(g_bell(), yellow, "g_bell")
    radar_icon(g_stopwatch(), (250, 250, 250, 255), "g_stopwatch", x_stopwatch)
    radar_icon(g_hourglass(), (190, 150, 90, 255), "g_timer", x_hourglass)
    radar_icon(g_picture(), (250, 250, 250, 255), "g_wallpaper", x_picture)
    radar_icon(g_speaker(), (240, 120, 40, 255), "g_sounds")
    radar_icon(g_padlock(), grey, "g_lock")
    radar_icon(g_power(), (250, 250, 250, 255), "g_power")
    radar_icon(g_info(), blue, "g_about", x_info)
    radar_icon(g_magnifier(), grey, "g_search")
    radar_icon(g_up(), green, "g_send")
    radar_icon(g_shutter(), (250, 250, 250, 255), "g_shutter", x_shutter)
    radar_icon(disc_shape(), (80, 150, 230, 255), "g_world", globe_extra)
    radar_icon(camera_shape(), (250, 250, 250, 255), "g_flip", x_flip)
    radar_icon(camera_shape(), (110, 110, 118, 255), "g_camera", camera_extra)
    camera_controls()



def wrench_shape():
    s = canvas()
    points = [(0.68, 0.08), (0.90, 0.13), (0.73, 0.31), (0.75, 0.40),
              (0.93, 0.23), (0.95, 0.43), (0.79, 0.55), (0.63, 0.53),
              (0.29, 0.90), (0.12, 0.87), (0.09, 0.70), (0.49, 0.35),
              (0.48, 0.19), (0.59, 0.10)]
    d = ImageDraw.Draw(s)
    d.polygon([(x * IS, y * IS) for x, y in points], fill=WHITE)
    return s


def icons(only_apps=False):
    radar_icon(handset_shape(), (70, 190, 60, 255), "app_phone")
    radar_icon(bubble_shape(), (250, 250, 250, 255), "app_text")
    radar_icon(person_shape(), (240, 190, 70, 255), "app_contacts")
    radar_icon(disc_shape(), (250, 250, 250, 255), "app_clock", clock_extra)
    radar_icon(disc_shape(), (80, 150, 230, 255), "app_internet", globe_extra)
    radar_icon(wrench_shape(), (220, 30, 25, 255), "app_settings")
    radar_icon(camera_shape(), (110, 110, 118, 255), "app_camera", camera_extra)
    radar_icon(g_picture(), (250, 250, 250, 255), "app_photos", x_picture)
    radar_icon(joystick_shape(), (150, 152, 160, 255), "app_games", joystick_extra)
    radar_icon(calculator_shape(), (70, 70, 76, 255), "app_calculator", calculator_extra)
    radar_icon(notes_shape(), (245, 225, 110, 255), "app_notes", notes_extra)
    radar_icon(map_shape(), (120, 190, 90, 255), "app_maps", map_extra)
    radar_icon(weather_shape(), (250, 200, 50, 255), "app_weather", weather_extra)
    radar_icon(stocks_shape(), (40, 40, 46, 255), "app_stocks", stocks_extra)
    radar_icon(radio_shape(), (210, 60, 55, 255), "app_radio", radio_extra)
    radar_icon(calendar_shape(), (245, 245, 248, 255), "app_calendar", calendar_extra)
    radar_icon(flashlight_shape(), (250, 200, 50, 255), "app_flashlight", flashlight_extra)
    if only_apps:
        return
    weather_glyphs()
    game_icons()
    glyphs()


def weather_shape():
    # The sun, and a cloud across its lower right.
    s = canvas()
    d = ImageDraw.Draw(s)
    d.ellipse((IS * 0.1, IS * 0.1, IS * 0.62, IS * 0.62), fill=(255, 255, 255, 255))
    cloud(d, 0.28, 0.44, 0.62, (255, 255, 255, 255))
    return s


def cloud(d, x, y, w, fill):
    """A cloud, its left at x and base at y + 0.3 w, in fractions of the icon."""
    h = w * 0.3
    d.rounded_rectangle((IS * x, IS * (y + h * 0.4), IS * (x + w), IS * (y + h)), radius=IS * h * 0.3, fill=fill)
    d.ellipse((IS * (x + w * 0.12), IS * (y + h * 0.05), IS * (x + w * 0.52), IS * (y + h * 0.95)), fill=fill)
    d.ellipse((IS * (x + w * 0.36), IS * (y - h * 0.4), IS * (x + w * 0.86), IS * (y + h * 0.95)), fill=fill)


def weather_extra(img, mask):
    d = ImageDraw.Draw(img)
    cloud(d, 0.28, 0.44, 0.62, (235, 238, 244, 255))


def stocks_shape():
    s = canvas()
    ImageDraw.Draw(s).rounded_rectangle((IS * 0.1, IS * 0.14, IS * 0.9, IS * 0.86), radius=IS * 0.08,
                                        fill=(255, 255, 255, 255))
    return s


def stocks_extra(img, mask):
    d = ImageDraw.Draw(img)
    for i in range(3):
        y = IS * (0.32 + i * 0.18)
        d.line((IS * 0.16, y, IS * 0.84, y), fill=(90, 90, 100, 255), width=int(IS * 0.012))
    pts = [(0.18, 0.72), (0.34, 0.58), (0.46, 0.64), (0.62, 0.4), (0.72, 0.46), (0.84, 0.24)]
    d.line([(IS * x, IS * y) for x, y in pts], fill=(90, 210, 90, 255), width=int(IS * 0.05), joint="curve")


def radio_shape():
    s = canvas()
    d = ImageDraw.Draw(s)
    d.rounded_rectangle((IS * 0.08, IS * 0.3, IS * 0.92, IS * 0.86), radius=IS * 0.08, fill=(255, 255, 255, 255))
    d.line((IS * 0.3, IS * 0.3, IS * 0.62, IS * 0.1), fill=(255, 255, 255, 255), width=int(IS * 0.05))
    return s


def radio_extra(img, mask):
    d = ImageDraw.Draw(img)
    for cx in (0.3, 0.7):
        r = 0.15
        d.ellipse((IS * (cx - r), IS * (0.58 - r), IS * (cx + r), IS * (0.58 + r)), fill=(30, 30, 34, 255))
        d.ellipse((IS * (cx - 0.05), IS * 0.53, IS * (cx + 0.05), IS * 0.63), fill=(120, 120, 130, 255))
    d.rectangle((IS * 0.42, IS * 0.38, IS * 0.58, IS * 0.46), fill=(250, 200, 60, 255))


def flashlight_shape():
    # A torch standing on end, its wide head at the top, and the light
    # fanning up out of it.
    s = canvas()
    d = ImageDraw.Draw(s)
    d.polygon([(IS * 0.28, IS * 0.3), (IS * 0.72, IS * 0.3), (IS * 0.62, IS * 0.5), (IS * 0.38, IS * 0.5)],
              fill=(255, 255, 255, 255))
    d.rounded_rectangle((IS * 0.38, IS * 0.46, IS * 0.62, IS * 0.94), radius=IS * 0.04, fill=(255, 255, 255, 255))
    d.polygon([(IS * 0.3, IS * 0.29), (IS * 0.7, IS * 0.29), (IS * 0.92, IS * 0.04), (IS * 0.08, IS * 0.04)],
              fill=(255, 255, 255, 255))
    return s


def flashlight_extra(img, mask):
    d = ImageDraw.Draw(img)
    # The beam: pale and bright over the torch's yellow.
    d.polygon([(IS * 0.3, IS * 0.29), (IS * 0.7, IS * 0.29), (IS * 0.92, IS * 0.04), (IS * 0.08, IS * 0.04)],
              fill=(255, 250, 215, 255))
    # The lens, and the switch on the handle.
    d.rectangle((IS * 0.3, IS * 0.27, IS * 0.7, IS * 0.32), fill=(255, 255, 255, 255))
    d.rounded_rectangle((IS * 0.45, IS * 0.6, IS * 0.55, IS * 0.7), radius=IS * 0.02, fill=(40, 40, 44, 255))


def calendar_shape():
    s = canvas()
    ImageDraw.Draw(s).rounded_rectangle((IS * 0.14, IS * 0.12, IS * 0.86, IS * 0.88), radius=IS * 0.06,
                                        fill=(255, 255, 255, 255))
    return s


def calendar_extra(img, mask):
    d = ImageDraw.Draw(img)
    d.rectangle((IS * 0.14, IS * 0.12, IS * 0.86, IS * 0.32), fill=(210, 50, 45, 255))
    for r in range(4):
        for c in range(5):
            x, y = IS * (0.22 + c * 0.12), IS * (0.42 + r * 0.11)
            fill = (47, 111, 208, 255) if (r, c) == (1, 3) else (60, 60, 66, 255)
            d.rectangle((x, y, x + IS * 0.07, y + IS * 0.06), fill=fill)


def weather_glyphs():
    """The Weather app's skies, drawn as the radar icons are."""
    sun = canvas()
    ImageDraw.Draw(sun).ellipse((IS * 0.2, IS * 0.2, IS * 0.8, IS * 0.8), fill=(255, 255, 255, 255))
    radar_icon(sun, (250, 200, 50, 255), "w_sun")
    radar_icon(sun, (240, 150, 70, 255), "w_haze")
    moon = canvas()
    dm = ImageDraw.Draw(moon)
    dm.ellipse((IS * 0.18, IS * 0.18, IS * 0.82, IS * 0.82), fill=(255, 255, 255, 255))
    dm.ellipse((IS * 0.38, IS * 0.08, IS * 0.98, IS * 0.68), fill=(0, 0, 0, 0))
    radar_icon(moon, (225, 225, 200, 255), "w_moon")
    radar_icon(weather_shape(), (250, 200, 50, 255), "w_partly", weather_extra)

    def cloudy(fill, name, extra=None):
        # The drops, bolt or bars are part of the outline too, so they are
        # not cut away with what lies outside the cloud.
        c = canvas()
        cloud(ImageDraw.Draw(c), 0.1, 0.3, 0.8, (255, 255, 255, 255))
        if extra:
            extra(c, None)
        radar_icon(c, fill, name, extra)

    def rain(img, mask):
        d = ImageDraw.Draw(img)
        for x in (0.3, 0.5, 0.7):
            d.line((IS * x, IS * 0.64, IS * (x - 0.06), IS * 0.86), fill=(70, 140, 235, 255), width=int(IS * 0.05))

    def bolt(img, mask):
        ImageDraw.Draw(img).polygon([(IS * 0.52, IS * 0.56), (IS * 0.36, IS * 0.78), (IS * 0.5, IS * 0.78),
                                     (IS * 0.42, IS * 0.96), (IS * 0.66, IS * 0.7), (IS * 0.52, IS * 0.7)],
                                    fill=(250, 210, 50, 255))

    def bars(img, mask):
        d = ImageDraw.Draw(img)
        for i, y in enumerate((0.66, 0.76, 0.86)):
            d.line((IS * (0.18 + i * 0.06), IS * y, IS * (0.82 - i * 0.04), IS * y), fill=(200, 200, 205, 255),
                   width=int(IS * 0.045))

    cloudy((215, 220, 230, 255), "w_cloud")
    cloudy((150, 158, 172, 255), "w_rain", rain)
    cloudy((95, 100, 115, 255), "w_storm", bolt)
    cloudy((185, 188, 195, 255), "w_fog", bars)
    cloudy((205, 170, 110, 255), "w_sand", bars)


def clock_face():
    """The Clock app's dial: white, black-edged, without hands."""
    S = 256 * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = S / 2
    d.ellipse((0, 0, S - 1, S - 1), fill=BLACK)
    fr = S * 0.46
    d.ellipse((c - fr, c - fr, c + fr, c + fr), fill=(245, 245, 245, 255))
    for i in range(60):
        a = math.radians(i * 6)
        major = i % 5 == 0
        l0 = fr * (0.78 if major else 0.88)
        d.line((c + math.cos(a) * l0, c + math.sin(a) * l0, c + math.cos(a) * fr * 0.95,
                c + math.sin(a) * fr * 0.95), fill=BLACK, width=int(S * (0.018 if major else 0.006)))
    finish(img, (256, 256), "clock_face")


def boot_mark():
    """The iFruit mark alone, white on clear, for the screen the phone shows
    while it starts up."""
    mark = ifruit_mark().resize((256, 256), Image.LANCZOS)
    img = Image.new("RGBA", (256, 256), (255, 255, 255, 0))
    img.putalpha(mark)
    img.save(os.path.join(OUT, "boot_mark.png"))


def cursor():
    """The phone's mouse cursor: the game's arrow shape, white edged in black,
    drawn at 32 x 32 - about the size it is shown at, so it is never shrunk
    far (which speckles its edge) - with a clear pixel all round."""
    S = 32 * SS * 2
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = S / 64.0
    pts = [(x * k, y * k) for x, y in [(4, 3), (4, 50), (15, 39), (23, 57), (31, 53), (23, 36), (39, 36)]]
    d.polygon(pts, fill=(0, 0, 0, 255))
    inner = [(x * k, y * k) for x, y in [(8, 12), (8, 41), (16, 33), (24, 50), (27, 48.5), (19, 31), (30, 31)]]
    d.polygon(inner, fill=(255, 255, 255, 255))
    img = img.resize((32, 32), Image.BOX)
    # The clear border, exactly: nothing a filter can pull in from the edge.
    px = img.load()
    for i in range(32):
        for x, y in ((i, 0), (i, 31), (0, i), (31, i)):
            px[x, y] = (0, 0, 0, 0)
    img.save(os.path.join(OUT, "cursor.png"))


def wallpapers():
    """SP-RP's wallpapers are landscape pictures with rounded corners on a
    transparent square. Each is cut, inside its corners, to the 2:3 upright
    screen so it fills it the way a phone's wallpaper does, and stored at
    128x256; the screen draws it back at 2:3, which undoes the squeeze."""
    src = os.path.join(ASSETS, "wallpapers")
    for i in range(27):
        im = Image.open(os.path.join(src, "bg_%d.png" % i)).convert("RGBA")
        l, t, r, b = im.getchannel("A").point(lambda v: 255 if v > 200 else 0).getbbox()
        # Clear of the rounded corners.
        inset = int(min(r - l, b - t) * 0.06)
        l, t, r, b = l + inset, t + inset, r - inset, b - inset
        w, h = r - l, b - t
        if w / h > 2 / 3:
            cw = h * 2 / 3
            l, r = l + (w - cw) / 2, l + (w + cw) / 2
        else:
            ch = w * 3 / 2
            t, b = t + (h - ch) / 2, t + (h + ch) / 2
        crop = im.crop((int(round(l)), int(round(t)), int(round(r)), int(round(b))))
        crop.convert("RGB").resize((128, 256), Image.LANCZOS).save(os.path.join(OUT, "wall_%d.png" % i))


def header():
    def fx(v):
        return (v + SIDE) / TEX_PU_W

    def fy(v):
        return v * K / BODY_TEX[1]

    def side(name, span, right):
        x0 = PU_W - 2.0 if right else -BUTTON_OUT
        x1 = PU_W + BUTTON_OUT if right else 2.0
        return "constexpr float %s[4] = {%.5ff, %.5ff, %.5ff, %.5ff};" % (
            name, fx(x0), fy(span[0]), fx(x1), fy(span[1]))

    lines = [
        "// Generated by tools/generate-phone-art.py. Do not edit - change the script",
        "// and run it again.",
        "//",
        "// Where things are on body.png, as fractions of the texture. The texture is",
        "// drawn at width w and height 2w; the body runs from kBodyLeft to kBodyRight, with",
        "// room either side for the side buttons.",
        "#pragma once",
        "",
        "namespace phone_art {",
        "constexpr float kBodyAspect = %.1ff;  // height / width" % (BODY_TEX[1] / BODY_TEX[0]),
        "constexpr float kScreenLeft = %.5ff;" % fx(SCREEN[0]),
        "constexpr float kScreenTop = %.5ff;" % fy(SCREEN[1]),
        "constexpr float kScreenRight = %.5ff;" % fx(SCREEN[2]),
        "constexpr float kScreenBottom = %.5ff;" % fy(SCREEN[3]),
        "constexpr float kHomeX = %.5ff;" % fx(HOME[0]),
        "constexpr float kHomeY = %.5ff;" % fy(HOME[1]),
        "constexpr float kHomeRadius = %.5ff;  // as a fraction of the width" % (HOME[2] / TEX_PU_W),
        "// The side buttons, left, top, right, bottom: the ring switch and the",
        "// volume buttons on the left edge, the sleep button on the right.",
        side("kRing", RING, False),
        side("kVolumeUp", VOL_UP, False),
        side("kVolumeDown", VOL_DOWN, False),
        side("kSleep", SLEEP, True),
        "constexpr float kBodyBottom = %.5ff;" % fy(TOP + BODY_H),
        "constexpr float kBodyLeft = %.5ff;" % fx(0),
        "constexpr float kBodyRight = %.5ff;" % fx(PU_W),
        "}  // namespace phone_art",
        "",
    ]
    with open(os.path.join(HERE, "..", "src", "phone_art.h"), "w", newline="\n") as f:
        f.write("\n".join(lines))


TRAINER_ASSETS = os.path.join(HERE, "..", "..", "..", "valkyrie-trainer", "assets")


def brands():
    """The logos the trainer's About page carries, for the phone's: each
    fitted, whole, into a transparent square, so the plugin draws them
    square."""
    for name, file, size in (("brand_valkyrie", "valkyrie.png", 128), ("brand_sprp", "sprp.png", 128),
                             ("brand_ssmp", "ssmp.png", 128)):
        src = Image.open(os.path.join(TRAINER_ASSETS, file)).convert("RGBA")
        f = size / max(src.size)
        src = src.resize((max(1, round(src.width * f)), max(1, round(src.height * f))), Image.LANCZOS)
        out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        out.alpha_composite(src, ((size - src.width) // 2, (size - src.height) // 2))
        out.save(os.path.join(OUT, name + ".png"))


if __name__ == "__main__":
    if args.apps_only:
        icons(only_apps=True)
        print('Wrote 17 home-screen app icons without touching other artwork.')
        raise SystemExit(0)
    body()
    brands()
    icons()
    clock_face()
    boot_mark()
    cursor()
    # SP-RP's own battery picture, used as it is in the status bar.
    Image.open(os.path.join(ASSETS, "sprp", "battery_green.png")).convert("RGBA").save(
        os.path.join(OUT, "battery_green.png"))
    # A studio reflection for the phone model's shine, for when the game's
    # own car reflection cannot be found: a bright sky, a dark floor, two
    # soft lamps.
    env = vgradient((128, 128), (235, 240, 250, 255), (30, 32, 38, 255))
    lamps = Image.new("L", (128, 128), 0)
    ImageDraw.Draw(lamps).ellipse((20, 14, 52, 40), fill=255)
    ImageDraw.Draw(lamps).ellipse((80, 22, 108, 46), fill=200)
    env.alpha_composite(fill_mask(lamps.filter(ImageFilter.GaussianBlur(6)), (255, 255, 255, 255)))
    env.save(os.path.join(OUT, "phone_env.png"))
    # The shape of a light's reflection, for the sun caught in the glass: a
    # soft round spot, white, fading to nothing at its edge. Where it falls
    # and how bright it is come from the game's sun at run time.
    glow = Image.new("RGBA", (64, 64), (255, 255, 255, 0))
    gp = glow.load()
    for gy in range(64):
        for gx in range(64):
            d = math.hypot(gx - 31.5, gy - 31.5) / 31.5
            gp[gx, gy] = (255, 255, 255, int(255 * max(0.0, 1.0 - d) ** 2.2))
    glow.save(os.path.join(OUT, "glow.png"))
    # Plain white, for shapes the plugin draws itself (clock hands).
    Image.new("RGBA", (8, 8), (255, 255, 255, 255)).save(os.path.join(OUT, "white.png"))
    wallpapers()
    header()
    print("wrote", len(os.listdir(OUT)), "textures to", os.path.abspath(OUT))
