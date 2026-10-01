"""Draws the phone's keypad skin (Skin=Keypad in
valkyrie-phone.ini) and writes it beside the rest of the phone's art:

    ../assets/generated/sm_*.png        the handset, the screen's background,
                                        the menu's tiles and pictures, and the
                                        start-up mark
    ../assets/model/textures-sm/*.png   the model's textures for the skin,
                                        packed as valkyrie-phone-model-sm.txd
    ../src/phone_art_sm.h               where the handset's buttons are

The handset is Valkyrie's keypad handset: black
glass over a black body, and under the screen three round keys ringed in lit
teal - A on the left, the round pad in the middle, minus on the right. It has
the iFruit's outline and its screen in the same place (src/phone_art.h), so
everything laid out on the screen stays where it is.

Unlike generate-phone-art.py this writes only its own files and leaves the
rest of the folder alone:

    python generate-sm-art.py
"""
import math
import os
import random
import shutil

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PHONE = os.path.join(HERE, "..")
OUT = os.path.join(PHONE, "assets", "generated")
MODEL_IN = os.path.join(PHONE, "assets", "model", "textures")
MODEL_OUT = os.path.join(PHONE, "assets", "model", "textures-sm")
HEADER = os.path.join(PHONE, "src", "phone_art_sm.h")

TEAL = (86, 214, 236)
TEAL_DEEP = (30, 128, 166)

# ---------------------------------------------------------------------------
# The handset: generate-phone-art.py's layout, in its phone units
# ---------------------------------------------------------------------------
PU_W, TOP, SIDE = 244.0, 4.0, 7.0
TEX_PU_W = PU_W + 2 * SIDE
BODY_TEX = (1024, 2048)
K = BODY_TEX[0] / TEX_PU_W
GLASS_X = 14.0
GLASS_W = PU_W - 2 * GLASS_X
SCREEN = (GLASS_X, TOP + 62.0, GLASS_X + GLASS_W, TOP + 62.0 + GLASS_W * 480.0 / 320.0)
HOME = (PU_W / 2, SCREEN[3] + 34.0, 21.0)
BODY_H = HOME[1] + HOME[2] + 18.0 - TOP
RING = (TOP + 66.0, TOP + 80.0)
VOL_UP = (TOP + 96.0, TOP + 126.0)
VOL_DOWN = (TOP + 134.0, TOP + 164.0)
SLEEP = (TOP + 88.0, TOP + 132.0)
BUTTON_OUT = 4.2
EAR = (PU_W / 2 - 24.0, TOP + 44.0, PU_W / 2 + 24.0, TOP + 50.0)

# The three keys under the screen: the pad where the iFruit's home button
# is, a little larger, and A and minus either side of it.
PAD = (PU_W / 2, HOME[1], 27.0)
KEY_A = (PU_W * 0.19, HOME[1] + 3.0, 14.0)
KEY_MINUS = (PU_W * 0.81, HOME[1] + 3.0, 14.0)


def ux(v):
    return (v + SIDE) * K


def u(v):
    return v * K


def rounded_box(px, py, cx, cy, hw, hh, r):
    """Signed distance to a rounded rectangle (negative inside) and the
    outward direction at the nearest edge, over arrays of points."""
    ax, ay = np.abs(px - cx), np.abs(py - cy)
    qx, qy = ax - (hw - r), ay - (hh - r)
    sx = np.where(px >= cx, 1.0, -1.0)
    sy = np.where(py >= cy, 1.0, -1.0)
    corner = (qx > 0) & (qy > 0)
    l = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
    ls = np.maximum(l, 1e-6)
    d = np.where(corner, l - r, np.maximum(qx, qy) - r)
    gx = np.where(corner, sx * qx / ls, np.where(qx > qy, sx, 0.0))
    gy = np.where(corner, sy * qy / ls, np.where(qx > qy, 0.0, sy))
    return d, gx, gy


def cover(d):
    return np.clip(0.5 - d, 0.0, 1.0)


def body():
    """The same construction as generate-phone-art.py's body(): colour,
    normals (x right, y up) and material (R gloss, G smoothness, B metal,
    A mirror), worked out from the shapes' distances."""
    TW, TH = BODY_TEX
    ys, xs = np.mgrid[0:TH, 0:TW].astype(np.float64)
    px, py = xs + 0.5, ys + 0.5

    rgb = np.zeros((TH, TW, 3))
    alpha = np.zeros((TH, TW))
    nx = np.zeros((TH, TW))
    ny = np.zeros((TH, TW))
    mat = np.zeros((TH, TW, 4))

    def tilt(dx, dy, a):
        s = np.sin(a)
        return dx * s, -dy * s

    PLASTIC = np.array([30.0, 31.0, 35.0])
    PLASTIC_M = np.array([205, 215, 0, 150])
    GLASS = np.array([9.0, 9.0, 11.0])
    GLASS_M = np.array([255, 245, 0, 255])

    # The side buttons, behind the body: black plastic capsules.
    for top, bottom, right in ((RING[0], RING[1], False), (VOL_UP[0], VOL_UP[1], False),
                               (VOL_DOWN[0], VOL_DOWN[1], False), (SLEEP[0], SLEEP[1], True)):
        out = u(BUTTON_OUT)
        x0 = ux(PU_W) - u(6) if right else ux(0) - out
        x1 = ux(PU_W) + out if right else ux(0) + u(6)
        bd, bx, by = rounded_box(px, py, (x0 + x1) / 2, (u(top) + u(bottom)) / 2, (x1 - x0) / 2,
                                 (u(bottom) - u(top)) / 2, u(3))
        m = bd < 0.5
        t = np.where(bd < 0, np.minimum(1.0, -bd / u(2.4)), 0.0)
        ang = 1.25 * (1 - t) ** 1.6
        shade = 0.7 + 0.3 * t
        tx, ty = tilt(bx, by, ang)
        a = cover(bd)
        for c in range(3):
            rgb[..., c] = np.where(m, 46.0 * shade, rgb[..., c])
        alpha = np.where(m, a, alpha)
        nx = np.where(m, tx, nx)
        ny = np.where(m, ty, ny)
        mat = np.where(m[..., None], PLASTIC_M, mat)

    # The body: a black plastic rim rolling off round the edge, and inside
    # it one pane of black glass.
    ocx, ocy = ux(PU_W / 2), u(TOP + BODY_H / 2)
    d, gx, gy = rounded_box(px, py, ocx, ocy, u(PU_W / 2), u(BODY_H / 2), u(32))
    a_body = cover(d)
    inside = -d
    bezel, roll, seam = u(5.0), u(4.2), u(0.6)
    on_rim = inside < bezel
    t = np.clip(inside / roll, 0, 1)
    rnx, rny = tilt(gx, gy, 1.35 * (1 - t) ** 1.8)
    rim_shade = 0.4 + 0.6 * np.clip(inside / u(1.2), 0, 1)
    rim_rgb = PLASTIC[None, None, :] * rim_shade[..., None]
    sd = np.abs(inside - (bezel - seam * 0.5))
    f = np.clip(1 - sd / seam, 0, 1)
    rim_rgb = rim_rgb * (1 - 0.6 * f[..., None])
    g_in = inside - bezel
    gnx, gny = tilt(gx, gy, 0.35 * np.maximum(0.0, 1 - g_in / u(1.4)) ** 2)
    b_rgb = np.where(on_rim[..., None], rim_rgb, GLASS[None, None, :])
    b_nx = np.where(on_rim, rnx, gnx)
    b_ny = np.where(on_rim, rny, gny)
    b_m = np.where(on_rim[..., None], PLASTIC_M, GLASS_M)

    glass = ~on_rim
    # The display, black under the glass, in a hairline frame.
    s = (ux(SCREEN[0]), u(SCREEN[1]), ux(SCREEN[2]), u(SCREEN[3]))
    sd_box, _, _ = rounded_box(px, py, (s[0] + s[2]) / 2, (s[1] + s[3]) / 2, (s[2] - s[0]) / 2,
                               (s[3] - s[1]) / 2, 0.5)
    frame = glass & (np.abs(sd_box - u(1.2)) < u(0.5))
    b_rgb = np.where(frame[..., None], np.array([34.0, 36.0, 40.0]), b_rgb)
    b_rgb = np.where((glass & (sd_box < 0))[..., None], np.array([4.0, 4.0, 5.0]), b_rgb)

    # The earpiece slot.
    ecx, ecy = (ux(EAR[0]) + ux(EAR[2])) / 2, (u(EAR[1]) + u(EAR[3])) / 2
    ed, ex, ey = rounded_box(px, py, ecx, ecy, (ux(EAR[2]) - ux(EAR[0])) / 2, (u(EAR[3]) - u(EAR[1])) / 2,
                             (u(EAR[3]) - u(EAR[1])) / 2)
    ear = glass & (ed < 0.5)
    fe = cover(ed)
    lift = 0.5 + 0.5 * np.sin(px * 1.9) * np.sin(py * 1.9)
    grille = np.stack([34 + 10 * lift, 34 + 10 * lift, 38 + 10 * lift], -1)
    b_rgb = np.where(ear[..., None], GLASS * (1 - fe[..., None]) + grille * fe[..., None], b_rgb)
    enx, eny = tilt(-ex, -ey, 0.6 * np.maximum(0.0, 1 - np.abs(ed) / u(0.8)))
    b_nx = np.where(ear, enx, b_nx)
    b_ny = np.where(ear, eny, b_ny)

    # The three keys: glossy black domes standing out of the glass, a ring
    # of teal light round each.
    for kx, ky, kr in (PAD, KEY_A, KEY_MINUS):
        cx, cy, r = ux(kx), u(ky), u(kr)
        hd = np.hypot(px - cx, py - cy)
        key = glass & (hd < r + 0.5)
        fk = cover(hd - r)
        dx, dy = (px - cx) / np.maximum(hd, 1e-3), (py - cy) / np.maximum(hd, 1e-3)
        # A narrow gap in the glass round it, then the dome rising.
        gap = u(1.3)
        e = np.clip((hd - (r - gap)) / gap, 0, 1)
        dome = np.clip(hd / (r - gap), 0, 1)
        knx, kny = tilt(dx, dy, np.where(hd > r - gap, -0.9 * e, 0.55 * dome ** 2))
        k_rgb = np.where((hd > r - gap)[..., None], np.array([6.0, 6.0, 7.0]),
                         np.array([22.0, 22.0, 25.0]) * (1.1 - 0.35 * dome[..., None]))
        k_rgb = GLASS * (1 - fk[..., None]) + k_rgb * fk[..., None]
        b_rgb = np.where(key[..., None], k_rgb, b_rgb)
        b_nx = np.where(key, knx, b_nx)
        b_ny = np.where(key, kny, b_ny)
        b_m = np.where(key[..., None], np.array([225, 200, 0, 190]), b_m)

    for c in range(3):
        rgb[..., c] = np.where(a_body > 0, b_rgb[..., c] * a_body + rgb[..., c] * (1 - a_body), rgb[..., c])
    nx = np.where(a_body > 0, b_nx * a_body + nx * (1 - a_body), nx)
    ny = np.where(a_body > 0, b_ny * a_body + ny * (1 - a_body), ny)
    mat = np.where((a_body > 0)[..., None], b_m, mat)
    alpha = np.where(a_body > 0, a_body + alpha * (1 - a_body), alpha)

    colour = Image.fromarray(np.dstack([rgb, alpha * 255]).round().clip(0, 255).astype(np.uint8), "RGBA")

    # The lit teal: rings, and the marks on the keys, drawn four times over
    # and brought down, with a soft glow round them.
    SS = 4
    big =Image.new("L", (TW * SS, TH * SS))
    d = ImageDraw.Draw(big)

    def circle(cx, cy, r, width):
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=255, width=int(width))

    def bar(x0, y0, x1, y1):
        d.rounded_rectangle((x0, y0, x1, y1), radius=min(x1 - x0, y1 - y0) / 2, fill=255)

    s4 = SS
    for kx, ky, kr in (PAD, KEY_A, KEY_MINUS):
        cx, cy = ux(kx) * s4, u(ky) * s4
        circle(cx, cy, u(kr - 2.2) * s4, u(1.5) * s4)
    # The pad: a short dash at each of its four sides.
    cx, cy, r = ux(PAD[0]) * s4, u(PAD[1]) * s4, u(PAD[2]) * s4
    L, T_ = u(6.0) * s4, u(1.7) * s4
    off = r * 0.62
    bar(cx - off - L / 2, cy - T_ / 2, cx - off + L / 2, cy + T_ / 2)
    bar(cx + off - L / 2, cy - T_ / 2, cx + off + L / 2, cy + T_ / 2)
    bar(cx - T_ / 2, cy - off - L / 2, cx + T_ / 2, cy - off + L / 2)
    bar(cx - T_ / 2, cy + off - L / 2, cx + T_ / 2, cy + off + L / 2)
    # Minus.
    cx, cy = ux(KEY_MINUS[0]) * s4, u(KEY_MINUS[1]) * s4
    bar(cx - u(5.5) * s4, cy - u(1.0) * s4, cx + u(5.5) * s4, cy + u(1.0) * s4)
    # A.
    cx, cy = ux(KEY_A[0]) * s4, u(KEY_A[1]) * s4
    font = ImageFont.truetype("arialbd.ttf", int(u(15) * s4))
    d.text((cx, cy + u(0.6) * s4), "A", font=font, fill=255, anchor="mm")
    light = big.resize((TW, TH), Image.LANCZOS)
    glow = light.filter(ImageFilter.GaussianBlur(u(2.0)))
    glow = glow.point(lambda v: min(255, int(v * 1.6)))
    lit = Image.new("RGBA", (TW, TH), TEAL + (255,))
    halo = Image.new("RGBA", (TW, TH), TEAL_DEEP + (255,))
    halo.putalpha(glow.point(lambda v: int(v * 0.75)))
    lit.putalpha(light)
    # Only on the glass and keys, never past the body.
    body_a = colour.getchannel("A")
    solid = colour.copy()
    solid.putalpha(Image.new("L", (TW, TH), 255))
    solid.alpha_composite(halo)
    solid.alpha_composite(lit)
    solid.putalpha(body_a)
    colour = solid

    n = np.dstack([np.round(128 + 127 * nx), np.round(128 + 127 * ny), np.full((TH, TW), 255.0),
                   np.full((TH, TW), 255.0)]).clip(0, 255).astype(np.uint8)
    # The lit rings are matt: they glow rather than mirror.
    lm = np.asarray(light, dtype=np.float64)[..., None] / 255.0
    mat = mat * (1 - lm) + np.array([60, 60, 0, 0]) * lm
    colour.save(os.path.join(OUT, "sm_body.png"))
    Image.fromarray(n, "RGBA").save(os.path.join(OUT, "sm_phone_normal.png"))
    Image.fromarray(mat.round().clip(0, 255).astype(np.uint8), "RGBA").save(
        os.path.join(OUT, "sm_phone_material.png"))
    return colour


def header():
    def fx(v):
        return (v + SIDE) / TEX_PU_W

    def fy(v):
        return v / (TEX_PU_W * 2)

    lines = [
        "// Generated by tools/generate-sm-art.py. Do not edit - change the script",
        "// and run it again.",
        "//",
        "// The keypad handset's keys on sm_body.png, as fractions of",
        "// the texture (x of its width, y of its height, a radius of its width),",
        "// laid out as phone_art.h's body is: its screen is the iFruit's.",
        "#pragma once",
        "",
        "namespace phone_art_sm {",
        "// The round pad, where the iFruit's home button is.",
        f"constexpr float kPadX = {fx(PAD[0]):.5f}f;",
        f"constexpr float kPadY = {fy(PAD[1]):.5f}f;",
        f"constexpr float kPadRadius = {PAD[2] / TEX_PU_W:.5f}f;",
        "// A, left of it, and minus, right of it.",
        f"constexpr float kAX = {fx(KEY_A[0]):.5f}f;",
        f"constexpr float kAY = {fy(KEY_A[1]):.5f}f;",
        f"constexpr float kMinusX = {fx(KEY_MINUS[0]):.5f}f;",
        f"constexpr float kMinusY = {fy(KEY_MINUS[1]):.5f}f;",
        f"constexpr float kKeyRadius = {KEY_A[2] / TEX_PU_W:.5f}f;",
        "}  // namespace phone_art_sm",
        "",
    ]
    with open(HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


# ---------------------------------------------------------------------------
# The screen: its background, the menu's tiles and their pictures
# ---------------------------------------------------------------------------
def background():
    """Black, with the cold blue haze the phone's display has round what is
    lit on it, and a frost of fine grain."""
    W = H = 512
    rng = np.random.default_rng(4)
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
    u_, v_ = xs / W - 0.5, (ys / H - 0.47) * 1.25
    r = np.hypot(u_, v_)
    haze = np.exp(-(r / 0.36) ** 2)
    top = np.exp(-((ys / H) / 0.10) ** 2) * 0.35
    grain = rng.normal(0, 1, (H // 2, W // 2))
    grain = np.asarray(Image.fromarray(((grain + 4) * 32).clip(0, 255).astype(np.uint8)).resize(
        (W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(1.2)), dtype=np.float64) / 255.0 - 0.5
    streaks = rng.normal(0, 1, (H // 16, W))
    streaks = np.asarray(Image.fromarray(((streaks + 4) * 32).clip(0, 255).astype(np.uint8)).resize(
        (W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(3)), dtype=np.float64) / 255.0 - 0.5
    frost = 1.0 + grain * 0.5 + streaks * 0.6
    base = np.array([3.0, 6.0, 10.0])
    glow = np.array([28.0, 86.0, 122.0])
    img = base + (glow * (haze * 0.85 + top)[..., None]) * frost[..., None]
    edge = np.clip(1.0 - (np.maximum(np.abs(u_) * 2, np.abs(ys / H - 0.5) * 2) - 0.75) * 2.0, 0.55, 1.0)
    img *= edge[..., None]
    Image.fromarray(img.clip(0, 255).astype(np.uint8), "RGB").save(os.path.join(OUT, "sm_back.png"))


def tile(name, top, bottom, rim, highlight):
    S = 512
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    grad = Image.new("RGBA", (1, S))
    for y in range(S):
        t = y / (S - 1)
        grad.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    grad = grad.resize((S, S))
    rng = np.random.default_rng(len(name))
    noise = rng.normal(0, 6, (S // 4, S // 4))
    noise = Image.fromarray((noise + 128).clip(0, 255).astype(np.uint8)).resize((S, S), Image.BICUBIC)
    grad = Image.merge("RGBA", [ImageChops.add(c, noise, 1, -128) for c in grad.convert("RGB").split()] +
                       [Image.new("L", (S, S), 255)])
    mask = Image.new("L", (S, S))
    ImageDraw.Draw(mask).rounded_rectangle((8, 8, S - 9, S - 9), radius=34, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((8, 8, S - 9, S - 9), radius=34, outline=rim + (255,), width=14)
    d.line((40, 26, S - 40, 26), fill=highlight + (200,), width=8)
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(OUT, name + ".png"))


def glyph(name, draw):
    """A pictogram, white on clear, drawn at 512 and brought down to 128."""
    S = 512
    m = Image.new("L", (S, S))
    d = ImageDraw.Draw(m)
    draw(m, d)
    img = Image.new("RGBA", (S, S), (255, 255, 255, 255))
    img.putalpha(m)
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(OUT, "sm_" + name + ".png"))


def thick(d, pts, w, fill=255):
    d.line(pts, fill=fill, width=int(w), joint="curve")
    for x, y in (pts[0], pts[-1]):
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=fill)


def g_phone(m, d):
    # The handset: a curved grip over the top left, the earpiece and the
    # mouthpiece turned in at its ends.
    cx, cy, R = 396, 396, 262
    d.arc((cx - R - 38, cy - R - 38, cx + R + 38, cy + R + 38), 176, 274, fill=255, width=76)
    d.rounded_rectangle((cx - 64, cy - R - 40, cx + 56, cy - R + 70), radius=30, fill=255)
    d.rounded_rectangle((cx - R - 40, cy - 64, cx - R + 70, cy + 56), radius=30, fill=255)


def g_text(m, d):
    d.rectangle((72, 136, 440, 380), fill=255)
    d.line((72, 136, 256, 282, 440, 136), fill=0, width=24, joint="curve")
    d.line((72, 380, 206, 262), fill=0, width=16)
    d.line((440, 380, 306, 262), fill=0, width=16)


def g_contacts(m, d):
    d.rounded_rectangle((150, 64, 410, 448), radius=22, fill=255)
    d.rectangle((188, 64, 202, 448), fill=0)
    for i in range(5):
        y = 104 + i * 76
        d.rounded_rectangle((104, y, 176, y + 28), radius=12, fill=255)
        d.rectangle((150, y - 8, 164, y + 36), fill=0)
    d.rectangle((244, 150, 364, 176), fill=0)
    d.rectangle((244, 214, 334, 236), fill=0)


def g_camera(m, d):
    d.rounded_rectangle((60, 160, 452, 404), radius=32, fill=255)
    d.rounded_rectangle((176, 108, 336, 180), radius=16, fill=255)
    d.ellipse((256 - 92, 286 - 92, 256 + 92, 286 + 92), fill=0)
    d.ellipse((256 - 64, 286 - 64, 256 + 64, 286 + 64), fill=255)
    d.ellipse((256 - 26, 286 - 26, 256 + 26, 286 + 26), fill=0)
    d.rectangle((378, 190, 420, 214), fill=0)


def g_photos(m, d):
    d.rectangle((160, 84, 440, 320), outline=255, width=30)
    d.rectangle((58, 150, 382, 440), fill=0)
    d.rectangle((74, 166, 366, 424), fill=255)
    d.rectangle((104, 196, 336, 394), fill=0)
    d.polygon([(104, 394), (178, 300), (226, 350), (272, 304), (336, 372), (336, 394)], fill=255)
    d.ellipse((268, 222, 314, 268), fill=255)


def g_maps(m, d):
    pts = []
    for i in range(41):
        t = i / 40
        x = 96 + 250 * t + 60 * math.sin(t * math.pi * 1.4)
        y = 420 - 270 * t + 40 * math.sin(t * math.pi * 2.0)
        pts.append((x, y))
    for i in range(0, 40, 5):
        thick(d, pts[i:i + 3], 30)
    x, y = 388, 126
    thick(d, [(x - 48, y - 48), (x + 48, y + 48)], 36)
    thick(d, [(x - 48, y + 48), (x + 48, y - 48)], 36)


def g_internet(m, d):
    d.ellipse((76, 76, 436, 436), fill=255)
    d.ellipse((186, 76, 326, 436), outline=0, width=18)
    d.line((256, 76, 256, 436), fill=0, width=18)
    d.line((76, 256, 436, 256), fill=0, width=18)
    d.arc((40, -40, 472, 196), 20, 160, fill=0, width=18)
    d.arc((40, 316, 472, 552), 200, 340, fill=0, width=18)


def g_games(m, d):
    d.rounded_rectangle((60, 170, 452, 350), radius=90, fill=255)
    d.ellipse((66, 250, 212, 420), fill=255)
    d.ellipse((300, 250, 446, 420), fill=255)
    d.rectangle((108, 250, 196, 276), fill=0)
    d.rectangle((139, 219, 165, 307), fill=0)
    for x, y in ((348, 282), (396, 236)):
        d.ellipse((x - 22, y - 22, x + 22, y + 22), fill=0)


def g_clock(m, d):
    d.ellipse((70, 70, 442, 442), fill=255)
    d.ellipse((104, 104, 408, 408), fill=0)
    for a in range(0, 360, 90):
        x, y = 256 + 126 * math.cos(math.radians(a)), 256 + 126 * math.sin(math.radians(a))
        d.ellipse((x - 16, y - 16, x + 16, y + 16), fill=255)
    thick(d, [(256, 256), (256, 148)], 30)
    thick(d, [(256, 256), (338, 300)], 30)


def g_calculator(m, d):
    d.rounded_rectangle((116, 62, 396, 450), radius=26, fill=255)
    d.rectangle((148, 96, 364, 176), fill=0)
    for r in range(3):
        for c in range(3):
            x, y = 148 + c * 78, 212 + r * 76
            d.rectangle((x, y, x + 60, y + 56), fill=0)


def g_notes(m, d):
    for i, end in enumerate((420, 372, 420, 330, 398)):
        y = 96 + i * 68
        d.rectangle((84, y, 128, y + 40), fill=255)
        d.rectangle((158, y + 4, end, y + 36), fill=255)


def g_settings(m, d):
    # A screwdriver one way, a spanner the other.
    thick(d, [(128, 128), (262, 262)], 26)
    thick(d, [(292, 292), (400, 400)], 72)
    thick(d, [(136, 390), (330, 196)], 56)
    d.ellipse((310, 70, 450, 210), fill=255)
    d.ellipse((350, 110, 410, 170), fill=0)
    d.polygon([(380, 140), (470, 60), (470, 0), (400, 0), (380, 40)], fill=0)
    d.polygon([(380, 140), (500, 80), (512, 160)], fill=0)


def g_weather(m, d):
    cx, cy = 186, 186
    d.ellipse((cx - 72, cy - 72, cx + 72, cy + 72), fill=255)
    for a in range(0, 360, 45):
        c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
        thick(d, [(cx + 100 * c, cy + 100 * s), (cx + 140 * c, cy + 140 * s)], 24)
    cloud = [((250, 320), 84), ((348, 296), 100), ((420, 352), 62), ((170, 364), 58)]
    for (x, y), r in cloud:
        d.ellipse((x - r - 22, y - r - 22, x + r + 22, y + r + 22), fill=0)
    d.rounded_rectangle((110, 330, 480, 440), radius=40, fill=0)
    for (x, y), r in cloud:
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    d.rounded_rectangle((132, 352, 462, 420), radius=34, fill=255)


def g_stocks(m, d):
    d.rectangle((76, 80, 102, 432), fill=255)
    d.rectangle((76, 406, 440, 432), fill=255)
    for i, top in enumerate((320, 262, 210, 130)):
        x = 136 + i * 76
        d.rectangle((x, top, x + 52, 390), fill=255)


def g_radio(m, d):
    thick(d, [(140, 196), (360, 84)], 18)
    d.ellipse((344, 68, 378, 102), fill=255)
    d.rounded_rectangle((60, 190, 452, 420), radius=32, fill=255)
    d.ellipse((166 - 74, 306 - 74, 166 + 74, 306 + 74), fill=0)
    for r_ in (48,):
        d.ellipse((166 - r_, 306 - r_, 166 + r_, 306 + r_), fill=255)
    d.ellipse((166 - 20, 306 - 20, 166 + 20, 306 + 20), fill=0)
    d.rectangle((286, 236, 414, 280), fill=0)
    for x in (314, 386):
        d.ellipse((x - 24, 326, x + 24, 374), fill=0)


def g_calendar(m, d):
    d.rounded_rectangle((76, 106, 436, 440), radius=24, fill=255)
    d.rectangle((76, 184, 436, 200), fill=0)
    for x in (156, 332):
        d.rectangle((x - 30, 60, x + 30, 150), fill=0)
        d.rounded_rectangle((x - 16, 70, x + 16, 140), radius=12, fill=255)
    for r in range(3):
        for c in range(4):
            x, y = 110 + c * 78, 228 + r * 66
            d.rectangle((x, y, x + 54, y + 46), fill=0)


def g_flashlight(m, d):
    d.polygon([(164, 176), (348, 176), (312, 276), (200, 276)], fill=255)
    d.rounded_rectangle((204, 262, 308, 452), radius=16, fill=255)
    d.rectangle((238, 306, 274, 346), fill=0)
    for (x0, y0), (x1, y1) in (((256, 140), (256, 60)), ((196, 146), (150, 80)), ((316, 146), (362, 80))):
        thick(d, [(x0, y0), (x1, y1)], 22)
    rot = m.rotate(-30, resample=Image.BICUBIC, center=(256, 256))
    m.paste(rot)


GLYPHS = {"phone": g_phone, "text": g_text, "contacts": g_contacts, "camera": g_camera, "photos": g_photos,
          "maps": g_maps, "internet": g_internet, "games": g_games, "clock": g_clock,
          "calculator": g_calculator, "notes": g_notes, "settings": g_settings, "weather": g_weather,
          "stocks": g_stocks, "radio": g_radio, "calendar": g_calendar, "flashlight": g_flashlight}


def boot_mark():
    """The start-up mark: the pad's ring of teal light, alone."""
    S = 512
    m = Image.new("L", (S, S))
    d = ImageDraw.Draw(m)
    d.ellipse((76, 76, 436, 436), outline=255, width=34)
    for x0, y0, x1, y1 in ((120, 244, 196, 268), (316, 244, 392, 268), (244, 120, 268, 196), (244, 316, 268, 392)):
        d.rounded_rectangle((x0, y0, x1, y1), radius=12, fill=255)
    img = Image.new("RGBA", (S, S), (255, 255, 255, 255))
    img.putalpha(m)
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(OUT, "sm_boot.png"))


# ---------------------------------------------------------------------------
# The model's textures for the skin
# ---------------------------------------------------------------------------
BODY_LEFT, BODY_RIGHT, BODY_BOTTOM = 0.02713, 0.97287, 0.89729


def model_textures(colour):
    os.makedirs(MODEL_OUT, exist_ok=True)
    w, h = colour.size
    crop = colour.crop((round(BODY_LEFT * w), 0, round(BODY_RIGHT * w), round(BODY_BOTTOM * h)))
    flat = Image.new("RGBA", crop.size, (9, 9, 11, 255))
    flat.alpha_composite(crop)
    flat.convert("RGB").resize((512, 1024), Image.LANCZOS).save(os.path.join(MODEL_OUT, "vp_front.png"))

    # The screen as the phone first shows it: the menu.
    W, H = 320, 480
    img = Image.open(os.path.join(OUT, "sm_back.png")).convert("RGBA").resize((W, H), Image.LANCZOS)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype("arialbd.ttf", 22)
    fs = ImageFont.truetype("arialbd.ttf", 15)
    for i in range(5):
        hh = 4 + i * 2.5
        d.rectangle((8 + i * 5, 16 - hh, 11 + i * 5, 16), fill=(186, 214, 70))
    d.rectangle((W - 38, 5, W - 12, 16), outline=(235, 235, 235), width=2)
    d.rectangle((W - 11, 8, W - 9, 13), fill=(235, 235, 235))
    d.rectangle((W - 35, 8, W - 15, 13), fill=(186, 214, 70))
    d.text((W / 2, 50), "MESSAGES", font=f, fill="white", anchor="mm")
    tile_img = Image.open(os.path.join(OUT, "sm_tile.png")).convert("RGBA").resize((82, 82), Image.LANCZOS)
    on_img = Image.open(os.path.join(OUT, "sm_tile_on.png")).convert("RGBA").resize((82, 82), Image.LANCZOS)
    order = ["contacts", "phone", "photos", "maps", "text", "camera", "settings", "calendar", "notes"]
    for i, name in enumerate(order):
        x, y = 32 + (i % 3) * 88, 88 + (i // 3) * 88
        on = name == "text"
        img.alpha_composite(on_img if on else tile_img, (x, y))
        g = Image.open(os.path.join(OUT, "sm_" + name + ".png")).convert("RGBA").resize((54, 54), Image.LANCZOS)
        tint = Image.new("RGBA", g.size, (255, 255, 255, 255) if on else (12, 40, 56, 255))
        tint.putalpha(g.getchannel("A"))
        img.alpha_composite(tint, (x + 14, y + 14))
    d.text((14, H - 30), "Select", font=fs, fill="white")
    d.text((W - 14, H - 30), "Off", font=fs, fill="white", anchor="ra")
    img.convert("RGB").resize((256, 512), Image.LANCZOS).save(os.path.join(MODEL_OUT, "vp_screen.png"))

    # Back: black soft plastic with a faint grain.
    rng = np.random.default_rng(9)
    g = rng.normal(0, 3.0, (1024, 512)) + 26
    back = np.dstack([g, g, g + 3]).clip(0, 255).astype(np.uint8)
    Image.fromarray(back, "RGB").filter(ImageFilter.GaussianBlur(0.8)).save(os.path.join(MODEL_OUT, "vp_back.png"))
    # The rim and buttons: dark gunmetal rather than polished steel.
    img = Image.new("RGB", (32, 64))
    dd = ImageDraw.Draw(img)
    for y in range(64):
        t = y / 63
        c = int(92 - 40 * abs(t - 0.35) * 1.6)
        dd.line((0, y, 31, y), fill=(c, c, c + 4))
    img.save(os.path.join(MODEL_OUT, "vp_chrome.png"))
    Image.new("RGB", (8, 8), (16, 16, 18)).save(os.path.join(MODEL_OUT, "vp_black.png"))
    Image.new("RGB", (64, 16), (40, 40, 44)).save(os.path.join(MODEL_OUT, "vp_button.png"))
    # The insides and the lens are the phone's own.
    for name in ("vp_lens", "vp_battery", "vp_board", "vp_chip", "vp_shield"):
        shutil.copy2(os.path.join(MODEL_IN, name + ".png"), os.path.join(MODEL_OUT, name + ".png"))


if __name__ == "__main__":
    header()
    colour = body()
    background()
    tile("sm_tile", (74, 178, 208), (36, 124, 162), (22, 84, 114), (150, 226, 244))
    tile("sm_tile_on", (196, 246, 255), (104, 214, 240), (226, 252, 255), (255, 255, 255))
    for name, fn in GLYPHS.items():
        glyph(name, fn)
    boot_mark()
    model_textures(colour)
    print("done")
