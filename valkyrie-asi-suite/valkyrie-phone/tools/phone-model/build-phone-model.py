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
# Hollow, as the real one is: a 1 mm wall, open to the front, where the
# insides sit.
BACK_WALL = -T / 2 + 1.1
cavity = helper(box("cavity", (W - 3.0, 12.0, H - 3.0), (0, BACK_WALL + 6.0, 0), "vp_back", corner=CORNER - 1.5))
back = box("back", shell_size, (0, back_y, 0), "vp_back", corner=CORNER, edge=3.2, edge_segs=4)
boolean(back, cut_box, "DIFFERENCE")
boolean(back, cavity, "DIFFERENCE")
cap = box("antenna cap", shell_size, (0, back_y, 0), "vp_black", corner=CORNER, edge=3.2, edge_segs=4)
boolean(cap, cut_box, "INTERSECT")
boolean(cap, cavity, "DIFFERENCE")

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

# The insides. Never seen in the game - the shell and the glass close over
# them - and kept to plain blocks, some 300 vertices in all, so the model
# stays as light as a weapon's. There for the model's own sake, and for
# pictures of it taken apart. Each is laid out as in the first iPhone: the
# display's module under the glass, the logic board across the top half
# with the camera behind it, the battery below, the speaker and the dock
# connector at the bottom under the antenna cap.
screen_w, screen_h = abs(fx(su1) - fx(su0)), abs(fz(sv1) - fz(sv0))
screen_x, screen_z = (fx(su0) + fx(su1)) / 2, (fz(sv0) + fz(sv1)) / 2
box("lcd module", (screen_w + 3.0, 2.0, screen_h + 3.0), (screen_x, 4.0, screen_z), "vp_shield")
box("logic board", (W - 10.0, 1.0, H * 0.34), (0, -1.6, H * 0.22), "vp_board")
box("processor", (12.0, 1.0, 12.0), (8.0, -0.6, H * 0.30), "vp_chip")
box("memory", (10.0, 1.0, 8.0), (-10.0, -0.6, H * 0.31), "vp_chip")
box("shield can", (24.0, 1.2, 13.0), (0, -0.5, H * 0.12), "vp_shield")
box("camera module", (8.0, 3.0, 8.0), (-W / 2 + 10.0, BACK_WALL + 1.5, H / 2 - 10.0), "vp_shield")
box("sim tray", (14.0, 1.6, 2.4), (16.0, -2.8, H / 2 - 3.0), "vp_chrome")
box("battery", (W - 12.0, 3.4, H * 0.44), (0, BACK_WALL + 1.7, -H * 0.14), "vp_battery")
box("speaker", (13.0, 2.4, 5.0), (-17.0, -3.5, -H / 2 + 8.0), "vp_shield")
box("dock connector", (18.0, 3.0, 3.0), (0, -2.0, -H / 2 + 3.4), "vp_shield")
cylinder("vibration motor", 4.0, 2.4, (18.0, -3.6, -H / 2 + 12.0), "vp_shield")

for ob in HELPERS:
    ob.hide_viewport = True
    ob.hide_render = True

# ---------------------------------------------------------------------------
# Out to a .dff
# ---------------------------------------------------------------------------


# The insides' pictures (the battery's label, the board) cover each part
# whole, from its own corners.
LOCAL = ("vp_battery", "vp_board", "vp_chip", "vp_shield")
PART_BOUNDS = {}


def uv(texture, x, y, z, part=None):
    """Where a point of each kind of part is on its texture: flat across the
    front for the front and screen, across the back for the back, and down
    the length for the rest."""
    if texture in LOCAL and part in PART_BOUNDS:
        (x0, z0), (x1, z1) = PART_BOUNDS[part]
        return 1.0 - (x - x0) / max(x1 - x0, 1e-3), 1.0 - (z - z0) / max(z1 - z0, 1e-3)
    u, v = 0.5 - x / W, 0.5 - z / H  # the body as the front sees it
    if texture == "vp_screen":
        return (u - su0) / (su1 - su0), (v - sv0) / (sv1 - sv0)
    if texture == "vp_back":
        return 1.0 - u, v
    return u, v


depsgraph = bpy.context.evaluated_depsgraph_get()
for ob in bpy.data.objects:
    if ob.type == "MESH" and ob not in HELPERS:
        ev = ob.evaluated_get(depsgraph)
        pts = [ev.matrix_world @ v.co for v in ev.to_mesh().vertices]
        PART_BOUNDS[ob.name] = ((min(q.x for q in pts), min(q.z for q in pts)), (max(q.x for q in pts), max(q.z for q in pts)))
        ev.to_mesh_clear()

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if "explode" in ARGS:
    # "-- [detailed] explode <out.png> [<screen.png>]": the parts laid out
    # along the phone's depth, front to back, each where it sits in the phone
    # and only moved straight out - pushed back together they are the phone.
    import json
    from bpy_extras.object_utils import world_to_camera_view
    from mathutils import Vector
    out = ARGS[ARGS.index("explode") + 1]
    home = ARGS[ARGS.index("explode") + 2] if len(ARGS) > ARGS.index("explode") + 2 else None
    tex_dir = os.path.normpath(os.path.join(HERE, "..", "..", "assets", "model", "textures"))
    LAYERS = [("glass", ["front"]), ("display", ["screen"]), ("rim", ["rim"]), ("lcd", ["lcd module"]),
              ("board", ["logic board", "processor", "memory", "shield can", "camera module", "sim tray", "speaker",
                         "dock connector", "vibration motor"]),
              ("battery", ["battery"]),
              ("back", ["back", "antenna cap", "ring switch", "volume up", "volume down", "sleep button", "camera"])]
    GAP = 26.0
    shine = {"vp_chrome": (1.0, 0.12), "vp_front": (0.0, 0.05), "vp_back": (0.9, 0.35), "vp_lens": (0.0, 0.05),
             "vp_shield": (0.8, 0.4), "vp_battery": (0.5, 0.4), "vp_board": (0.0, 0.55), "vp_chip": (0.0, 0.4)}
    for name, m in MATERIALS.items():
        m.use_nodes = True
        bsdf = m.node_tree.nodes["Principled BSDF"]
        tex = m.node_tree.nodes.new("ShaderNodeTexImage")
        path = home if (name == "vp_screen" and home) else os.path.join(tex_dir, name + ".png")
        tex.image = bpy.data.images.load(path)
        m.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        metal, rough = shine.get(name, (0.0, 0.5))
        bsdf.inputs["Metallic"].default_value = metal
        bsdf.inputs["Roughness"].default_value = rough
        if name == "vp_screen":
            m.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = 1.4
        if name == "vp_front":
            bsdf.inputs["Alpha"].default_value = 0.5
            bsdf.inputs["Coat Weight"].default_value = 1.0
    layer_of = {part: (i, layer) for i, (layer, parts) in enumerate(LAYERS) for part in parts}
    placed = {}
    for ob in list(bpy.data.objects):
        if ob.type != "MESH" or ob in HELPERS:
            continue
        ev = ob.evaluated_get(depsgraph)
        me = bpy.data.meshes.new_from_object(ev)
        me.transform(ev.matrix_world)
        # Only this mapping: a primitive's own default one would be the one
        # its texture reads.
        while me.uv_layers:
            me.uv_layers.remove(me.uv_layers[0])
        layer = me.uv_layers.new()
        for poly in me.polygons:
            tex = me.materials[poly.material_index].name if me.materials else "vp_black"
            for li in poly.loop_indices:
                co = me.vertices[me.loops[li].vertex_index].co
                t = uv(tex, co.x, co.y, co.z, ob.name)
                layer.data[li].uv = (t[0], 1.0 - t[1])
        i, lname = layer_of.get(ob.name, (len(LAYERS) - 1, "back"))
        nob = bpy.data.objects.new(ob.name + " apart", me)
        bpy.context.scene.collection.objects.link(nob)
        nob.location = (0, (len(LAYERS) - 1 - i) * GAP, 0)
        placed.setdefault(lname, []).append(nob)
        ob.hide_render = True
    scene = bpy.context.scene
    world = bpy.data.worlds.new("w")
    scene.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.2, 0.21, 0.24, 1)
    bpy.context.view_layer.update()
    pts = [o.matrix_world @ v.co for objs in placed.values() for o in objs for v in o.data.vertices]
    centre = sum(pts, Vector()) / len(pts)

    def light(loc, energy, size=60):
        ld = bpy.data.lights.new("l", "AREA")
        ld.energy = energy * 6
        ld.size = size
        o = bpy.data.objects.new("l", ld)
        scene.collection.objects.link(o)
        o.location = centre + Vector(loc)
        o.rotation_euler = (centre - o.location).to_track_quat("-Z", "Y").to_euler()

    light((-90, 120, 90), 6000)
    light((110, 30, 70), 3500)
    light((0, -130, 30), 2500)
    cam = bpy.data.cameras.new("c")
    cam.type = "ORTHO"
    camera = bpy.data.objects.new("c", cam)
    scene.collection.objects.link(camera)
    scene.camera = camera
    camera.location = centre + Vector((1.2, 0.9, 0.32)).normalized() * 400
    camera.rotation_euler = (centre - camera.location).to_track_quat("-Z", "Y").to_euler()
    scene.render.resolution_x, scene.render.resolution_y = 2000, 1250
    bpy.context.view_layer.update()
    inv = camera.matrix_world.inverted()
    local = [inv @ q for q in pts]
    xs, ys = [q.x for q in local], [q.y for q in local]
    # Framed on the parts, with a margin, their middle in the middle.
    aspect = scene.render.resolution_x / scene.render.resolution_y
    cam.ortho_scale = max(max(xs) - min(xs), (max(ys) - min(ys)) * aspect) * 1.08
    camera.location = camera.matrix_world @ Vector(((max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2, 0))
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 128
    scene.cycles.use_denoising = True
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Standard"
    bpy.context.view_layer.update()
    spots = {}
    for lname, objs in placed.items():
        q = [world_to_camera_view(scene, camera, o.matrix_world @ v.co) for o in objs for v in o.data.vertices]
        mid = sum(q, Vector()) / len(q)
        top = min(q, key=lambda c: -c.y)
        bottom = min(q, key=lambda c: c.y)
        spots[lname] = {"mid": [mid.x, 1 - mid.y], "top": [top.x, 1 - top.y], "bottom": [bottom.x, 1 - bottom.y]}
    json.dump(spots, open(out.replace(".png", ".json"), "w"))
    scene.render.filepath = out
    bpy.ops.render.render(write_still=True)
    print("EXPLODED", out, flush=True)
    os._exit(0)

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
            t = uv(tex, p.x, p.y, p.z, ob.name)
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
