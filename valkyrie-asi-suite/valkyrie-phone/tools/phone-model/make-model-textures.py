"""The textures of the phone model CJ holds (build-phone-model.py), one for
each kind of part, into assets/model/textures:

    vp_front   the front, cut from the on-screen handset (generated/body.png),
               so the phone in his hand is the one on screen
    vp_screen  the home screen, lit (the model's screen glows by itself)
    vp_back    brushed aluminium with the iFruit mark
    vp_chrome  the polished steel of the rim and the buttons
    vp_black   the black plastic of the antenna cap
    vp_button  the side buttons, anodised, a colour each in its own quarter:
               sleep red, volume up bright blue, volume down dark blue, the
               ring switch orange (the phone splits them off the steel and
               plastic when it reads the model, and picks their quarter)
    vp_lens    the camera's lens

Run after tools/generate-phone-art.py:
    python tools/phone-model/make-model-textures.py
"""
import importlib.util
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PHONE = os.path.join(HERE, "..", "..")
GENERATED = os.path.join(PHONE, "assets", "generated")
OUT = os.path.join(PHONE, "assets", "model", "textures")

# Where the body is on body.png (src/phone_art.h).
BODY_LEFT, BODY_RIGHT, BODY_BOTTOM = 0.02713, 0.97287, 0.89729


def front():
    body = Image.open(os.path.join(GENERATED, "body.png")).convert("RGBA")
    w, h = body.size
    crop = body.crop((round(BODY_LEFT * w), 0, round(BODY_RIGHT * w), round(BODY_BOTTOM * h)))
    # Outside the rounded corners the picture is clear; the model's own
    # corners are rounded, so that never shows, but keep it the glass's black.
    flat = Image.new("RGBA", crop.size, (12, 12, 14, 255))
    flat.alpha_composite(crop)
    flat.convert("RGB").resize((512, 1024), Image.LANCZOS).save(os.path.join(OUT, "vp_front.png"))


def screen():
    """The home screen as the phone first shows it: the default wallpaper,
    the apps four across and the dock."""
    W, H = 320, 480
    img = Image.open(os.path.join(GENERATED, "wall_3.png")).convert("RGBA").resize((W, H), Image.LANCZOS)
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype("arialbd.ttf", 13)
        fs = ImageFont.truetype("arial.ttf", 11)
    except OSError:
        f = fs = ImageFont.load_default()
    d.rectangle((0, 0, W, 20), fill=(0, 0, 0, 235))
    d.text((7, 3), "iFruit", font=f, fill="white")
    d.text((W // 2 - 20, 3), "2:56 PM", font=f, fill="white")
    d.rectangle((W - 35, 6, W - 9, 15), outline="white")
    d.rectangle((W - 33, 8, W - 12, 13), fill=(90, 210, 60))
    apps = [("app_phone", "Phone"), ("app_text", "Text"), ("app_contacts", "Contacts"), ("app_photos", "Camera"),
            ("app_maps", "Maps"), ("app_internet", "Internet"), ("app_games", "Games"), ("app_clock", "Clock"),
            ("app_calculator", "Calculator"), ("app_notes", "Notes"), ("app_settings", "Settings")]
    cell = W / 4
    for i, (name, label) in enumerate(apps):
        cx, y = cell * (i % 4) + cell / 2, 34 + (i // 4) * 90
        icon = Image.open(os.path.join(GENERATED, name + ".png")).convert("RGBA").resize((57, 57), Image.NEAREST)
        img.alpha_composite(icon, (int(cx - 28), int(y)))
        tw = d.textlength(label, font=fs)
        d.text((cx - tw / 2, y + 60), label, font=fs, fill="white", stroke_width=1, stroke_fill="black")
    img.alpha_composite(Image.new("RGBA", (W, 92), (40, 42, 48, 200)), (0, H - 92))
    d.rectangle((0, H - 92, W, H - 90), fill=(200, 204, 212, 200))
    for i, name in enumerate(["app_phone", "app_text", "app_internet", "app_games"]):
        cx = W / 4 * i + W / 8
        icon = Image.open(os.path.join(GENERATED, name + ".png")).convert("RGBA").resize((57, 57), Image.NEAREST)
        img.alpha_composite(icon, (int(cx - 28), H - 82))
    img.convert("RGB").resize((256, 512), Image.LANCZOS).save(os.path.join(OUT, "vp_screen.png"))


def back():
    """Brushed aluminium, the lines running across, with the iFruit mark."""
    rng = random.Random(7)
    W, H = 512, 1024
    img = Image.new("L", (W, H))
    px = img.load()
    for y in range(H):
        row = rng.uniform(-10, 10)
        for x in range(W):
            px[x, y] = max(0, min(255, int(196 + row + rng.uniform(-6, 6))))
    img = img.filter(ImageFilter.BoxBlur(1)).convert("RGB")
    # The mark, a little above the middle, as on the first iPhone's back.
    spec = importlib.util.spec_from_file_location("art", os.path.join(HERE, "..", "generate-phone-art.py"))
    art_text = open(spec.origin, encoding="utf-8").read()
    # Only ifruit_mark() is wanted; loading the whole script would empty the
    # generated folder.
    ns = {"os": os, "Image": Image, "HERE": os.path.join(HERE, "..")}
    start = art_text.index("RADAR_LOGO = ")
    ns["RADAR_LOGO"] = eval(art_text[start:art_text.index("\n", start)].split("=", 1)[1].strip(), ns)
    fn = art_text[art_text.index("def ifruit_mark"):]
    exec(fn[:fn.index("\n\n\n")], ns)
    mark = ns["ifruit_mark"]().resize((192, 192), Image.LANCZOS)
    dark = Image.new("RGB", mark.size, (70, 72, 78))
    img.paste(dark, ((W - 192) // 2, 300), mark)
    try:
        f = ImageFont.truetype("arial.ttf", 44)
    except OSError:
        f = ImageFont.load_default()
    d = ImageDraw.Draw(img)
    tw = d.textlength("iFruit", font=f)
    d.text(((W - tw) / 2, 524), "iFruit", font=f, fill=(90, 92, 98))
    img.save(os.path.join(OUT, "vp_back.png"))


def chrome():
    img = Image.new("RGB", (32, 64))
    d = ImageDraw.Draw(img)
    for y in range(64):
        t = y / 63
        c = int(235 - 90 * abs(t - 0.35) * 1.6)
        d.line((0, y, 31, y), fill=(c, c, min(255, c + 4)))
    img.save(os.path.join(OUT, "vp_chrome.png"))


def solid(name, colour, size=8):
    Image.new("RGB", (size, size), colour).save(os.path.join(OUT, name + ".png"))


def buttons():
    img = Image.new("RGB", (64, 16))
    for i, colour in enumerate([(205, 35, 35), (60, 150, 255), (25, 55, 150), (255, 130, 20)]):
        img.paste(colour, (i * 16, 0, i * 16 + 16, 16))
    img.save(os.path.join(OUT, "vp_button.png"))


def lens():
    img = Image.new("RGB", (32, 32), (8, 8, 10))
    d = ImageDraw.Draw(img)
    d.ellipse((6, 6, 25, 25), fill=(20, 26, 44))
    d.ellipse((11, 11, 20, 20), fill=(4, 4, 8))
    d.ellipse((12, 11, 15, 14), fill=(120, 140, 190))
    img.save(os.path.join(OUT, "vp_lens.png"))


# The insides, never seen in the game (the shell closes over them) - there
# for the model's own sake, and for pictures of it taken apart.


def battery():
    """The cell: silver foil under a black printed label."""
    W, H = 256, 512
    rng = random.Random(3)
    img = Image.new("RGB", (W, H), (176, 178, 184))
    d = ImageDraw.Draw(img)
    for y in range(0, H, 2):
        c = 170 + rng.randint(-8, 8)
        d.line((0, y, W, y), fill=(c, c + 2, c + 6))
    d.rounded_rectangle((18, 40, W - 18, H - 40), radius=10, fill=(24, 24, 28))
    try:
        big = ImageFont.truetype("arialbd.ttf", 30)
        mid = ImageFont.truetype("arialbd.ttf", 17)
        small = ImageFont.truetype("arial.ttf", 13)
    except OSError:
        big = mid = small = ImageFont.load_default()
    d.text((W / 2, 110), "iFruit", font=big, fill=(235, 235, 235), anchor="mm")
    d.text((W / 2, 160), "Li-ion Polymer Battery", font=mid, fill=(220, 220, 220), anchor="mm")
    for i, line in enumerate(["3.7 V   1400 mAh   5.2 Wh", "Do not puncture, crush or burn.",
                              "Made in San Andreas", "Model  VP-1407"]):
        d.text((W / 2, 215 + i * 26), line, font=small, fill=(190, 190, 190), anchor="mm")
    for i in range(46):  # a barcode
        w = rng.choice((1, 1, 2, 3))
        x = 58 + i * 3
        d.rectangle((x, 360, x + w - 1, 410), fill=(230, 230, 230))
    d.text((W / 2, 425), "0 07410 28 20071", font=small, fill=(200, 200, 200), anchor="mm")
    img.save(os.path.join(OUT, "vp_battery.png"))


def board():
    """The logic board: green solder mask, copper traces and vias."""
    W = H = 256
    rng = random.Random(11)
    img = Image.new("RGB", (W, H), (18, 92, 48))
    d = ImageDraw.Draw(img)
    for _ in range(90):
        x, y = rng.randrange(W), rng.randrange(H)
        for _ in range(rng.randint(2, 5)):
            if rng.random() < 0.5:
                nx, ny = x + rng.randint(-60, 60), y
            else:
                nx, ny = x, y + rng.randint(-60, 60)
            d.line((x, y, nx, ny), fill=(40, 140, 70), width=2)
            x, y = nx, ny
        d.ellipse((x - 2, y - 2, x + 2, y + 2), fill=(200, 170, 80))
    for _ in range(40):
        x, y = rng.randrange(W), rng.randrange(H)
        d.rectangle((x, y, x + 5, y + 3), fill=(150, 120, 60))
    try:
        f = ImageFont.truetype("arial.ttf", 11)
    except OSError:
        f = ImageFont.load_default()
    d.text((8, H - 16), "VP M68  820-2071-A", font=f, fill=(230, 230, 220))
    img.save(os.path.join(OUT, "vp_board.png"))


def chip():
    """A processor: black epoxy, a printed part number, a dot at pin one."""
    img = Image.new("RGB", (64, 64), (20, 20, 22))
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype("arialbd.ttf", 9)
    except OSError:
        f = ImageFont.load_default()
    d.text((32, 24), "VK-A1", font=f, fill=(170, 170, 170), anchor="mm")
    d.text((32, 38), "412 MHz", font=f, fill=(140, 140, 140), anchor="mm")
    d.ellipse((6, 6, 10, 10), fill=(60, 60, 64))
    img.save(os.path.join(OUT, "vp_chip.png"))


def shield():
    """The shield cans and frames: brushed, darker steel."""
    rng = random.Random(5)
    img = Image.new("L", (64, 64))
    px = img.load()
    for y in range(64):
        row = rng.uniform(-6, 6)
        for x in range(64):
            px[x, y] = max(0, min(255, int(128 + row + rng.uniform(-5, 5))))
    img.convert("RGB").save(os.path.join(OUT, "vp_shield.png"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    front()
    screen()
    back()
    chrome()
    solid("vp_black", (22, 22, 24))
    buttons()
    lens()
    battery()
    board()
    chip()
    shield()
    print("textures in", os.path.normpath(OUT))
