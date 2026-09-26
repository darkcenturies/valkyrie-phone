"""Works out the phone signal the existing masts give across the map.

Model, per 100-unit cell (1 unit = 1 metre):
  - Okumura-Hata path loss, suburban correction, at the chosen frequency;
    base station height is the antenna's height above the phone's ground,
    between 30 and 200 m.
  - One knife-edge diffraction loss (ITU-R P.526) for the worst obstacle on
    the path, over mean terrain per cell, with the 4/3-earth bulge.
    Obstacles within 150 m of either end are ignored: buildings and trees
    right next to the phone are already in the Hata clutter term.
  - Signal while total loss <= 150 dB (a typical GSM maximum allowable path
    loss). Bars: <=120 dB 4, <=130 3, <=140 2, <=150 1.

Inputs: heightmap.npy from build_heightmap.py, masts.csv, and the game
folder (its Valkyrie-map-overview.txd gives the land outline and the picture
drawn under the result). Land the heightmap has no tiles for is treated as
flat ground at 20 m.

    python coverage.py <game folder> heightmap.npy [MHz]

Writes coverage-<MHz>.npy (path loss in dB, 100-unit cells, row 0 at
y = 16000, column 0 at x = -5000), coverage-<MHz>.png with and without
water, and prints the share of land and water in each band. The pictures are
made from the game's radar map: keep them on your own machine.
"""
import csv
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageEnhance, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
CELL_IN = 25
X0, X1, Y0, Y1 = -5000, 19500, -12500, 16000
RES = 100
ANTENNA = 50.0          # metres above the mast's placement or the ground
PHONE = 1.5
EARTH = 4 / 3 * 6371000.0
MAPL = 150.0
BARS = [150, 140, 130, 120]
SAMPLES = 128


def overview(game):
    data = open(os.path.join(game, "Valkyrie-map-overview.txd"), "rb").read()
    width, height = struct.unpack_from("<HH", data, 0x84)
    size = struct.unpack_from("<I", data, 0x8C)[0]
    return Image.frombytes("RGBA", (width, height), data[0x90:0x90 + size], "bcn", 1).convert("RGB")


def world_to_overview(x, y, pixels):
    scale = pixels / 48000
    return (x + 24000) * scale, (24000 - y) * scale


def shrink(a, f, fn):
    return fn(fn(a.reshape(a.shape[0] // f, f, a.shape[1] // f, f), axis=3), axis=1)


class Terrain:
    def __init__(self, heights, picture):
        h, w = heights.shape
        xs = X0 + (np.arange(w) + .5) * CELL_IN
        ys = Y1 - (np.arange(h) + .5) * CELL_IN
        px, py = world_to_overview(xs, ys, picture.width)
        rgb = np.asarray(picture).astype(int)[py.astype(int)][:, px.astype(int)]
        land = np.abs(rgb - rgb[0, 0]).sum(2) > 45
        ground = np.where(heights > -1e9, np.maximum(heights, 0), 0).astype(np.float32)
        ground[land & (heights <= -1e9)] = 20
        f = RES // CELL_IN
        self.h, self.w = h // f, w // f
        ground, land = ground[:self.h * f, :self.w * f], land[:self.h * f, :self.w * f]
        self.ground = shrink(ground, f, np.mean)
        self.land = shrink(land.astype(np.float32), f, np.mean) > 0.5
        self.cx = X0 + (np.arange(self.w) + .5) * RES
        self.cy = Y1 - (np.arange(self.h) + .5) * RES

    def at(self, x, y):
        return float(self.ground[int((Y1 - y) // RES), int((x - X0) // RES)])


def hata(km, base, mhz):
    km = np.maximum(km, 0.05)
    a = (1.1 * np.log10(mhz) - 0.7) * PHONE - (1.56 * np.log10(mhz) - 0.8)
    urban = (69.55 + 26.16 * np.log10(mhz) - 13.82 * np.log10(base) - a
             + (44.9 - 6.55 * np.log10(base)) * np.log10(km))
    return urban - 2 * np.log10(mhz / 28) ** 2 - 5.4


def knife_edge(v):
    return np.where(v > -0.78, 6.9 + 20 * np.log10(np.sqrt((v - 0.1) ** 2 + 1) + v - 0.1), 0.0)


def path_loss(t, x, y, top, mhz, reach=45000):
    wave = 300 / mhz
    out = np.full((t.h, t.w), np.inf, np.float32)
    iy0, ix0, r = int((Y1 - y) // RES), int((x - X0) // RES), int(reach // RES)
    yy, xx = np.mgrid[max(0, iy0 - r):min(t.h, iy0 + r + 1), max(0, ix0 - r):min(t.w, ix0 + r + 1)]
    dx, dy = t.cx[xx] - x, t.cy[yy] - y
    dist = np.hypot(dx, dy)
    keep = (dist <= reach) & (dist > RES)
    dx, dy, dist, cells = dx[keep], dy[keep], dist[keep], (yy[keep], xx[keep])
    phone = t.ground[cells] + PHONE
    loss = np.empty(len(dist), np.float32)
    step = (np.arange(1, SAMPLES) / SAMPLES)[None, :]
    for s in range(0, len(dist), 20000):
        part = slice(s, s + 20000)
        sx, sy = x + dx[part, None] * step, y + dy[part, None] * step
        ix = np.clip(((sx - X0) // RES).astype(int), 0, t.w - 1)
        iy = np.clip(((Y1 - sy) // RES).astype(int), 0, t.h - 1)
        d1 = dist[part, None] * step
        d2 = dist[part, None] - d1
        obstacle = t.ground[iy, ix] - d1 * d2 / (2 * EARTH)
        obstacle = np.where((d1 < 150) | (d2 < 150), -1e4, obstacle)
        clearance = obstacle - (top + (phone[part, None] - top) * step)
        v = clearance * np.sqrt(2 / wave * (d1 + d2) / (d1 * d2))
        base = np.clip(top - t.ground[cells][part], 30, 200)
        loss[part] = hata(dist[part] / 1000, base, mhz) + knife_edge(v.max(1))
    out[cells] = loss
    return out


def draw(t, loss, picture, sites, mhz, water, path):
    s = picture.width / 48000
    left, top = world_to_overview(X0, Y1, picture.width)
    right, bottom = world_to_overview(X1, Y0 + 500, picture.width)
    scale = 1.25
    base = picture.crop((int(left), int(top), int(right), int(bottom)))
    base = base.resize((int(base.width * scale), int(base.height * scale)), Image.LANCZOS)
    base = ImageEnhance.Brightness(ImageEnhance.Color(base).enhance(0.15)).enhance(0.85).convert("RGBA")
    level = np.zeros(loss.shape, np.uint8)
    for i, limit in enumerate(BARS):
        level[loss <= limit] = i + 1
    colours = {0: (20, 20, 20), 1: (215, 48, 39), 2: (252, 141, 89), 3: (145, 207, 96), 4: (26, 152, 80)}
    rgba = np.zeros(loss.shape + (4,), np.uint8)
    for k, c in colours.items():
        rgba[(level == k) & t.land] = c + (150,)
        if water:
            rgba[(level == k) & ~t.land] = c + (110,)
    layer = Image.fromarray(rgba, "RGBA")
    cell = RES * s * scale
    layer = layer.resize((int(round(layer.width * cell)), int(round(layer.height * cell))), Image.BILINEAR)
    canvas = Image.new("RGBA", base.size, (0, 0, 0, 0))
    canvas.paste(layer, (0, 0))
    out = Image.alpha_composite(base, canvas)
    pen = ImageDraw.Draw(out)
    bold = ImageFont.truetype("arialbd.ttf", 16)
    small = ImageFont.truetype("arial.ttf", 14)
    title = ImageFont.truetype("arialbd.ttf", 22)
    for site in sites:
        px, py = world_to_overview(site["x"], site["y"], picture.width)
        px, py = (px - left) * scale, (py - top) * scale
        pen.polygon([(px, py - 14), (px - 9, py + 9), (px + 9, py + 9)], fill=(255, 255, 255), outline=(0, 0, 0), width=2)
        pen.text((px + 12, py - 8), site["name"], font=bold, fill=(255, 255, 255), stroke_width=3, stroke_fill=(0, 0, 0))
    area = ~t.land if water else t.land
    share = [((loss <= b) & area).sum() / area.sum() * 100 for b in reversed(BARS)]
    rows = [("4 bars", colours[4], share[0]), ("3 bars", colours[3], share[1] - share[0]),
            ("2 bars", colours[2], share[2] - share[1]), ("1 bar", colours[1], share[3] - share[2]),
            ("No signal", colours[0], 100 - share[3])]
    x = out.width - 470
    pen.rectangle((x, 10, out.width - 10, 236), fill=(0, 0, 0))
    pen.text((x + 12, 16), f"Existing masts, {mhz:g} MHz" + (", land + water" if water else ""), font=title, fill=(255, 255, 255))
    pen.text((x + 12, 46), "Hata path loss + terrain diffraction, 50 m antennas", font=small, fill=(200, 200, 200))
    for i, (name, colour, pct) in enumerate(rows):
        y = 76 + i * 26
        pen.rectangle((x + 12, y, x + 36, y + 18), fill=colour, outline=(255, 255, 255))
        pen.text((x + 46, y), name, font=small, fill=(235, 235, 235))
        pen.text((out.width - 80, y), f"{pct:4.1f}%", font=small, fill=(235, 235, 235))
    pen.text((x + 12, 210), "Share of " + ("water" if water else "land") + " in each band", font=small, fill=(200, 200, 200))
    out.convert("RGB").save(path, optimize=True)


def main(game, heightmap, mhz):
    picture = overview(game)
    t = Terrain(np.load(heightmap), picture)
    sites = [dict(name=r["site"], x=float(r["x"]), y=float(r["y"]), z=float(r["z"]))
             for r in csv.DictReader(open(os.path.join(HERE, "masts.csv")))]
    best = np.full((t.h, t.w), np.inf, np.float32)
    for site in sites:
        top = max(site["z"], t.at(site["x"], site["y"])) + ANTENNA
        loss = path_loss(t, site["x"], site["y"], top, mhz)
        best = np.minimum(best, loss)
        print(f"{site['name']:<38} {((loss <= MAPL) & t.land).sum() / t.land.sum():6.1%} of land")
    for name, area in (("land", t.land), ("water", ~t.land)):
        bands = [f"{((best <= b) & area).sum() / area.sum():.1%}" for b in reversed(BARS)]
        print(f"{name}: at least 4/3/2/1 bars = {' / '.join(bands)}")
    np.save(f"coverage-{mhz:g}.npy", best)
    draw(t, best, picture, sites, mhz, False, f"coverage-{mhz:g}.png")
    draw(t, best, picture, sites, mhz, True, f"coverage-{mhz:g}-water.png")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 900.0)
