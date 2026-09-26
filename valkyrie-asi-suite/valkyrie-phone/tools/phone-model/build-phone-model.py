"""The phone model CJ holds, built part by part and written as a RenderWare
.dff for San Andreas:

    blender --factory-startup -b -P tools/phone-model/build-phone-model.py

Each part is its own material, named by its texture (make-model-textures.py),
so the phone can give each its own shine: the polished rim and buttons, the
glass front, the lit screen, the brushed aluminium back, the black antenna
cap and ring switch, the camera lens.

It is laid out as the phone on screen is (src/phone_art.h): the screen, the
home button, the ring switch and volume buttons down the left edge and the
sleep button on the right. It sits where the model it replaces sat, so CJ's
grip (HandTurn, HandOffset, HandFlip in the ini) carries over: the screen
faces +y, the top is +z, and the middle is at MIDDLE, in metres, from the
point the hand holds.

The .dff is written here rather than by an exporter add-on, so building it
needs only Blender. Needs Blender 4.1 or later (Mesh.corner_normals).
"""
import math
import os
import struct
import sys

import bmesh
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
# Built in about as many triangles as the game's own handheld models (its
# camera.dff has 420, its M4 532): three steps round each corner, every edge
# square, plain blocks for the buttons - in CJ's hand and on the screen
# alike. "-- detailed" builds the smooth one (8-step corners, rounded edges,
# some 3,200 triangles) to valkyrie-phone-model-detailed.dff instead.
LOW = "detailed" not in (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "assets", "model",
                                    "valkyrie-phone-model.dff" if LOW else "valkyrie-phone-model-detailed.dff"))

# Millimetres. The width and height are the on-screen handset's proportions
# (its body on body.png is 1.9 times as tall as it is wide, so its screen is
# 2:3 as the interface is) at the size of the model it replaces.
W, H, T = 68.0, 129.0, 13.0
CORNER = W * 0.14
MIDDLE = (0.078, 0.022, 0.0095)  # metres, from the grip

# Fractions of the body on body.png, left to right as the screen is seen and
# top to bottom (src/phone_art.h, over the body's own span).
BODY_L, BODY_R, BODY_B = 0.02713, 0.97287, 0.89729


def fx(u):
    """Across the body, as the front is seen, to x (the viewer's left is +x)."""
    return (0.5 - u) * W


def fz(v):
    """Down the body to z."""
    return (0.5 - v) * H


def art_y(v):
    return v / BODY_B  # a height on body.png to a fraction of the body


def art_x(u):
    return (u - BODY_L) / (BODY_R - BODY_L)


SCREEN = (art_x(0.08140), art_y(0.12791), art_x(0.91860), art_y(0.75581))
HOME = (0.5, art_y(0.82171), 0.08140 / (BODY_R - BODY_L) * W)  # u, v, radius in mm
RING = (art_y(0.13566), art_y(0.16279))
VOL_UP = (art_y(0.19380), art_y(0.25194))
VOL_DOWN = (art_y(0.26744), art_y(0.32558))
SLEEP = (art_y(0.17829), art_y(0.26357))

bpy.ops.wm.read_factory_settings(use_empty=True)

MATERIALS = {}


def material(texture):
    if texture not in MATERIALS:
        MATERIALS[texture] = bpy.data.materials.new(texture)
    return MATERIALS[texture]


def box(name, size, loc, texture, corner=0.0, edge=0.0, segs=8, edge_segs=3):
    """A box; `corner` rounds its corners as the front sees them, `edge`
    rounds every edge."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co.x *= size[0]
        v.co.y *= size[1]
        v.co.z *= size[2]
    if corner > 0:
        bw = bm.edges.layers.float.get("bevel_weight_edge") or bm.edges.layers.float.new("bevel_weight_edge")
        for e in bm.edges:
            d = e.verts[0].co - e.verts[1].co
            if abs(d.y) > 1e-6 and abs(d.x) < 1e-6 and abs(d.z) < 1e-6:
                e[bw] = 1.0
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(ob)
    ob.location = loc
    if LOW:
        segs = min(segs, 3)
        edge = 0.0
    if corner > 0:
        m = ob.modifiers.new("corners", "BEVEL")
        m.limit_method = "WEIGHT"
        m.width = corner
        m.segments = segs
        m.harden_normals = True
    if edge > 0:
        m = ob.modifiers.new("edges", "BEVEL")
        m.limit_method = "ANGLE"
        m.width = edge
        m.segments = edge_segs
        m.harden_normals = True
    me.materials.append(material(texture))
    for p in me.polygons:
        p.use_smooth = True
    return ob


def cylinder(name, r, depth, loc, texture, verts=32):
    if LOW:
        verts = 8
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=depth, location=loc, vertices=verts,
                                        rotation=(math.radians(90), 0, 0))
    ob = bpy.context.active_object
    ob.name = name
    ob.data.materials.append(material(texture))
    m = ob.modifiers.new("edges", "BEVEL")
    m.width = min(r, depth) * 0.3
    m.segments = 2
    m.harden_normals = True
    for p in ob.data.polygons:
        p.use_smooth = True
    return ob


def boolean(ob, other, operation):
    m = ob.modifiers.new(operation.lower(), "BOOLEAN")
    m.operation = operation
    m.object = other
    m.solver = "EXACT"
    return ob


HELPERS = []


def helper(ob):
    HELPERS.append(ob)
    return ob


# The back and sides: one rounded body, its bottom cut off as the black cap
# over the antenna, as the first iPhone's is.
back_y = -0.6
shell_size = (W, T - 1.2, H)
cap_h = H * 0.15
cut_box = helper(box("cap cut", (W + 4, T + 4, cap_h * 2), (0, back_y, -H / 2), "vp_back"))
back = box("back", shell_size, (0, back_y, 0), "vp_back", corner=CORNER, edge=3.2, edge_segs=4)
boolean(back, cut_box, "DIFFERENCE")
cap = box("antenna cap", shell_size, (0, back_y, 0), "vp_black", corner=CORNER, edge=3.2, edge_segs=4)
boolean(cap, cut_box, "INTERSECT")

# The polished rim round the front.
rim = box("rim", (W, 1.6, H), (0, T / 2 - 1.0, 0), "vp_chrome", corner=CORNER, edge=0.5)
boolean(rim, helper(box("rim inside", (W - 2.4, 4, H - 2.4), (0, T / 2 - 1.0, 0), "vp_chrome",
                        corner=CORNER - 1.2)), "DIFFERENCE")

# The glass over the whole front, and the lit screen under its surface.
glass = box("front", (W - 1.0, 0.6, H - 1.0), (0, T / 2 - 0.3, 0), "vp_front", corner=CORNER - 0.5, edge=0.25)
su0, sv0, su1, sv1 = SCREEN
bpy.ops.mesh.primitive_plane_add(size=1, rotation=(math.radians(-90), 0, 0),
                                 location=((fx(su0) + fx(su1)) / 2, T / 2 + 0.15, (fz(sv0) + fz(sv1)) / 2))
screen = bpy.context.active_object
screen.name = "screen"
screen.scale = (abs(fx(su1) - fx(su0)), abs(fz(sv1) - fz(sv0)), 1)
screen.data.materials.append(material("vp_screen"))

# The home button is drawn in the glass (vp_front, from body.png), as flush
# as the first iPhone's: a disc of its own shows a thin, glinting wall round
# it at the phone's size on screen.


def side_button(name, span, side, texture, depth=1.4):
    z0, z1 = fz(span[0]), fz(span[1])
    x = side * (W / 2 + depth / 2 - 0.4)
    return box(name, (depth, 3.2, abs(z1 - z0)), (x, back_y + 1.0, (z0 + z1) / 2), texture, edge=0.5)


side_button("ring switch", RING, +1, "vp_black")
side_button("volume up", VOL_UP, +1, "vp_chrome")
side_button("volume down", VOL_DOWN, +1, "vp_chrome")
side_button("sleep button", SLEEP, -1, "vp_chrome")

# The camera, top left of the back as it is seen from behind.
cylinder("camera", 2.6, 0.6, (-W / 2 + 10.0, -T / 2 - 0.05, H / 2 - 10.0), "vp_lens")

for ob in HELPERS:
    ob.hide_viewport = True
    ob.hide_render = True

# ---------------------------------------------------------------------------
# Out to a .dff
# ---------------------------------------------------------------------------


def uv(texture, x, y, z):
    """Where a point of each kind of part is on its texture: flat across the
    front for the front and screen, across the back for the back, and down
    the length for the rest."""
    u, v = 0.5 - x / W, 0.5 - z / H  # the body as the front sees it
    if texture == "vp_screen":
        return (u - su0) / (su1 - su0), (v - sv0) / (sv1 - sv0)
    if texture == "vp_back":
        return 1.0 - u, v
    return u, v


depsgraph = bpy.context.evaluated_depsgraph_get()
textures = []
vertices, index = [], {}
triangles = []  # (a, b, c, material)
for ob in bpy.data.objects:
    if ob.type != "MESH" or ob in HELPERS:
        continue
    ev = ob.evaluated_get(depsgraph)
    me = ev.to_mesh()
    me.calc_loop_triangles()
    mw = ev.matrix_world
    nm = mw.to_3x3().inverted().transposed()
    normals = me.corner_normals
    for tri in me.loop_triangles:
        tex = me.materials[tri.material_index].name if me.materials else "vp_black"
        if tex not in textures:
            textures.append(tex)
        mi = textures.index(tex)
        corners = []
        for loop in tri.loops:
            p = mw @ me.vertices[me.loops[loop].vertex_index].co
            n = (nm @ normals[loop].vector).normalized()
            t = uv(tex, p.x, p.y, p.z)
            key = (round(p.x, 4), round(p.y, 4), round(p.z, 4), round(n.x, 3), round(n.y, 3), round(n.z, 3),
                   round(t[0], 4), round(t[1], 4), mi)
            if key not in index:
                index[key] = len(vertices)
                vertices.append((p.copy(), n.copy(), t, tex))
            corners.append(index[key])
        triangles.append((*corners, mi))
    ev.to_mesh_clear()

if len(vertices) > 65535:
    raise SystemExit("too many vertices for one RenderWare geometry: %d" % len(vertices))

VERSION = 0x1803FFFF


def chunk(kind, data):
    return struct.pack("<III", kind, len(data), VERSION) + data


def string(s):
    b = s.encode("ascii") + b"\0"
    return chunk(0x02, b + b"\0" * (-len(b) % 4))


def to_game(p):
    return tuple(MIDDLE[i] + p[i] * 0.001 for i in range(3))


positions = [to_game(v[0]) for v in vertices]
centre = tuple(sum(p[i] for p in positions) / len(positions) for i in range(3))
radius = max(math.dist(p, centre) for p in positions)

# The screen lights itself; everything else takes the game's light.
prelit = b"".join(struct.pack("<4B", *((150, 150, 150, 255) if v[3] == "vp_screen" else (0, 0, 0, 255)))
                  for v in vertices)
uvs = b"".join(struct.pack("<2f", *v[2]) for v in vertices)
# RpTriangle on disk: vertIndex[1], vertIndex[0], matIndex, vertIndex[2].
tris = b"".join(struct.pack("<4H", b, a, m, c) for a, b, c, m in triangles)
verts = b"".join(struct.pack("<3f", *p) for p in positions)
norms = b"".join(struct.pack("<3f", *v[1]) for v in vertices)
# rpGEOMETRYPOSITIONS | TEXTURED | PRELIT | NORMALS | LIGHT | MODULATEMATERIALCOLOR, one UV set.
flags = 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | (1 << 16)
geometry_struct = (struct.pack("<4I", flags, len(triangles), len(vertices), 1) + prelit + uvs + tris
                   + struct.pack("<4f", *centre, radius) + struct.pack("<2I", 1, 1) + verts + norms)


def material_chunk(texture):
    # Flags, white, unused, textured; ambient, specular, diffuse.
    body = chunk(0x01, struct.pack("<I4BII3f", 0, 255, 255, 255, 255, 0, 1, 1.0, 1.0, 1.0))
    # Linear filtering, wrapped in u and v.
    tex = chunk(0x01, struct.pack("<I", 0x1102)) + string(texture) + string("") + chunk(0x03, b"")
    return chunk(0x07, body + chunk(0x06, tex) + chunk(0x03, b""))


material_list = chunk(0x08, chunk(0x01, struct.pack("<I", len(textures)) + struct.pack("<i", -1) * len(textures))
                      + b"".join(material_chunk(t) for t in textures))
meshes = b""
for mi in range(len(textures)):
    idx = [i for a, b, c, m in triangles if m == mi for i in (a, b, c)]
    meshes += struct.pack("<2I", len(idx), mi) + struct.pack("<%dI" % len(idx), *idx)
bin_mesh = chunk(0x50E, struct.pack("<3I", 0, len(textures), len(triangles) * 3) + meshes)
geometry = chunk(0x0F, chunk(0x01, geometry_struct) + material_list
                 + chunk(0x03, bin_mesh + chunk(0x253F2FD, struct.pack("<I", 0))))

frame_struct = struct.pack("<I", 1) + struct.pack("<9f", 1, 0, 0, 0, 1, 0, 0, 0, 1) + struct.pack("<3f", 0, 0, 0) \
    + struct.pack("<iI", -1, 0x00020003)
frame_list = chunk(0x0E, chunk(0x01, frame_struct) + chunk(0x03, chunk(0x253F2FE, b"cellphone")))
geometry_list = chunk(0x1A, chunk(0x01, struct.pack("<I", 1)) + geometry)
atomic = chunk(0x14, chunk(0x01, struct.pack("<4I", 0, 0, 5, 0)) + chunk(0x03, b""))
clump = chunk(0x10, chunk(0x01, struct.pack("<3I", 1, 0, 0)) + frame_list + geometry_list + atomic + chunk(0x03, b""))

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "wb").write(clump)
print("wrote %s: %d vertices, %d triangles, %d materials (%s)" % (OUT, len(vertices), len(triangles), len(textures),
                                                                  ", ".join(textures)), flush=True)
os._exit(0)
