"""Read the phone model's .dff back and render it from the front and the back,
with its textures, to check what the game will be given:

    blender --factory-startup -b -P tools/phone-model/preview-dff.py -- <out.png>
"""
import math
import os
import struct
import sys

import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.join(HERE, "..", "..", "assets", "model")
OUT = sys.argv[sys.argv.index("--") + 1]

b = open(os.path.join(MODEL, "valkyrie-phone-model.dff"), "rb").read()


def chunks(off, end):
    while off + 12 <= end:
        t, s, _ = struct.unpack_from("<III", b, off)
        yield t, off + 12, s
        off += 12 + s


def find(kind, off=0, end=None, found=None):
    found = [] if found is None else found
    for t, body, s in chunks(off, len(b) if end is None else end):
        if t == kind:
            found.append((body, s))
        if t in (0x10, 0x1A, 0x0F, 0x08, 0x07, 0x06, 0x03):
            find(kind, body, body + s, found)
    return found


geo, _ = find(0x0F)[0]
st = geo + 12
fmt, nt, nv, _ = struct.unpack_from("<4I", b, st)
p = st + 16
if fmt & 0x08:
    p += 4 * nv
uv = [struct.unpack_from("<2f", b, p + 8 * i) for i in range(nv)]
p += 8 * nv
tris = [struct.unpack_from("<4H", b, p + 8 * i) for i in range(nt)]
p += 8 * nt + 16 + 8
verts = [struct.unpack_from("<3f", b, p + 12 * i) for i in range(nv)]
p += 12 * nv
normals = [struct.unpack_from("<3f", b, p + 12 * i) for i in range(nv)]
names = []
for body, s in find(0x06):
    for t, sb, ss in chunks(body, body + s):
        if t == 0x02:
            names.append(b[sb:sb + ss].split(b"\0")[0].decode())
            break

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
me = bpy.data.meshes.new("phone")
faces = [(v0, v1, v2) for v1, v0, m, v2 in tris]
me.from_pydata([Vector(v) * 100 for v in verts], [], faces)
layer = me.uv_layers.new()
for poly, (v1, v0, m, v2) in zip(me.polygons, tris):
    poly.material_index = m
    poly.use_smooth = True
    for li, vi in zip(poly.loop_indices, (v0, v1, v2)):
        layer.data[li].uv = (uv[vi][0], 1.0 - uv[vi][1])
me.normals_split_custom_set([normals[me.loops[i].vertex_index] for i in range(len(me.loops))])
for name in names:
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt_ = m.node_tree
    bsdf = nt_.nodes["Principled BSDF"]
    tex = nt_.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(os.path.join(MODEL, "textures", name + ".png"))
    nt_.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    shine = {"vp_chrome": (1.0, 0.1), "vp_front": (0.0, 0.08), "vp_back": (0.9, 0.35), "vp_lens": (0.0, 0.05)}
    metal, rough = shine.get(name, (0.0, 0.5))
    bsdf.inputs["Metallic"].default_value = metal
    bsdf.inputs["Roughness"].default_value = rough
    if name == "vp_screen":
        nt_.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.0
    me.materials.append(m)
ob = bpy.data.objects.new("phone", me)
scene.collection.objects.link(ob)
centre = sum((Vector(v) * 100 for v in verts), Vector()) / len(verts)

world = bpy.data.worlds.new("w")
scene.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.35, 0.37, 0.4, 1)


def light(loc, energy):
    ld = bpy.data.lights.new("l", "AREA")
    ld.energy = energy
    ld.size = 20
    o = bpy.data.objects.new("l", ld)
    scene.collection.objects.link(o)
    o.location = centre + Vector(loc)
    o.rotation_euler = (centre - o.location).to_track_quat("-Z", "Y").to_euler()


light((-30, 40, 30), 4000)
light((30, -40, 20), 3000)

cam = bpy.data.cameras.new("c")
cam.type = "ORTHO"
cam.ortho_scale = 17
camera = bpy.data.objects.new("c", cam)
scene.collection.objects.link(camera)
scene.camera = camera
scene.render.engine = "CYCLES"
scene.cycles.samples = 32
scene.cycles.use_denoising = True
scene.render.resolution_x, scene.render.resolution_y = 700, 900
scene.view_settings.view_transform = "Standard"

# "-- <out.png> icon": one picture on a clear background, lying on the
# diagonal as the game's weapon icons do, for the HUD's weapon slot.
if "icon" in sys.argv[sys.argv.index("--") + 1:]:
    scene.render.film_transparent = True
    scene.render.resolution_x = scene.render.resolution_y = 512
    cam.ortho_scale = 15.5
    ob.rotation_euler = (0.0, math.radians(-38.0), 0.0)
    bpy.context.view_layer.update()
    centre = ob.matrix_world @ centre
    camera.location = centre + Vector((-0.35, 1.0, 0.3)).normalized() * 40
    camera.rotation_euler = (centre - camera.location).to_track_quat("-Z", "Y").to_euler()
    scene.render.filepath = OUT
    bpy.ops.render.render(write_still=True)
    print("ICON", OUT, flush=True)
    os._exit(0)

# "-- <out.png> pe-icon": the shape and shading only, for a HUD icon in
# A flat preview: every part a
# plain light grey, the screen and the home button darker so it still reads
# as the phone, lit from the top right, on a clear background, leaning into
# the top right as the game's own phones do.
if "pe-icon" in sys.argv[sys.argv.index("--") + 1:]:
    for m in me.materials:
        bsdf = m.node_tree.nodes["Principled BSDF"]
        for link in list(m.node_tree.links):
            if link.to_node == bsdf:
                m.node_tree.links.remove(link)
        dark = m.name in ("vp_screen", "vp_black", "vp_lens")
        grey = 0.4 if dark else 0.8
        bsdf.inputs["Base Color"].default_value = (grey, grey, grey, 1)
        bsdf.inputs["Metallic"].default_value = 0.0
        bsdf.inputs["Roughness"].default_value = 0.35 if dark else 0.6
        bsdf.inputs["Emission Strength"].default_value = 0.0
    for o in [o for o in scene.objects if o.type == "LIGHT"]:
        bpy.data.objects.remove(o)
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.25, 0.25, 0.25, 1)
    scene.render.film_transparent = True
    scene.render.resolution_x = scene.render.resolution_y = 512
    cam.ortho_scale = 24.0
    ob.rotation_euler = (0.0, math.radians(-30.0), 0.0)
    bpy.context.view_layer.update()
    centre = ob.matrix_world @ centre
    light((25, 30, 35), 5000)
    light((-30, 20, -10), 700)
    camera.location = centre + Vector((1.1, 1.0, 0.25)).normalized() * 40
    camera.rotation_euler = (centre - camera.location).to_track_quat("-Z", "Y").to_euler()
    scene.render.filepath = OUT
    bpy.ops.render.render(write_still=True)
    print("PEICON", OUT, flush=True)
    os._exit(0)

shots = []
for i, direction in enumerate([(-0.55, 1.0, 0.25), (0.6, -1.0, 0.2)]):
    camera.location = centre + Vector(direction).normalized() * 40
    camera.rotation_euler = (centre - camera.location).to_track_quat("-Z", "Y").to_euler()
    path = OUT.replace(".png", "_%d.png" % i)
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    shots.append(path)
print("PREVIEW", shots, flush=True)
os._exit(0)
