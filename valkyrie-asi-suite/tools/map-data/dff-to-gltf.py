#!/usr/bin/env python3
"""
dff-to-gltf.py -- Turn the game's models into something a browser can draw.

deploy/dff-geometry.py already reads vertices and triangles out of a RenderWare
.dff, because that is all collision needed. A 3D map needs more: where each
texture sits on the surface, and which texture that is. Without those every
building arrives as a grey lump.

RenderWare sections are [type:u32][size:u32][libraryId:u32][body]:

    0x0010 Clump
      0x001A Geometry List
        0x000F Geometry
          0x0001 Struct     flags, triangle count, vertex count, then - in this
                            order and only if the flags say so - prelit colours,
                            texture coordinates, triangles, and finally the
                            morph target holding the vertices and normals.
          0x0008 Material List
            0x0007 Material
              0x0006 Texture
                0x0002 String   the texture's name

A triangle is stored as (b, a, materialIndex, c), which is not the order anyone
expects, and the material index is what ties a face to its texture.
"""

import json
import struct
import sys

CLUMP, GEOMETRY_LIST, GEOMETRY = 0x0010, 0x001A, 0x000F
STRUCT, MATERIAL_LIST, MATERIAL, TEXTURE, STRING = 0x0001, 0x0008, 0x0007, 0x0006, 0x0002
EXTENSION, BIN_MESH = 0x0003, 0x050E

FLAG_TEXTURED = 0x04
FLAG_PRELIT = 0x08
FLAG_NORMALS = 0x10
FLAG_TEXTURED2 = 0x80


def sections(data, start, end):
    """Walk the sections at one level."""
    at = start
    while at + 12 <= end:
        kind, size, _lib = struct.unpack_from("<III", data, at)
        body = at + 12
        if body + size > end:
            break
        yield kind, body, size
        at = body + size


def find(data, start, end, want):
    for kind, body, size in sections(data, start, end):
        if kind == want:
            return body, size
    return None, 0


def material_slots(data, start, size):
    """Expand the material-list references into the geometry's slot order."""
    body, body_size = find(data, start, start + size, MATERIAL_LIST)
    if body is None:
        return []
    children = list(sections(data, body, body + body_size))
    records = iter((b, s) for k, b, s in children if k == MATERIAL)
    header = next(((b, s) for k, b, s in children if k == STRUCT), None)
    if header is None:
        raise ValueError("material list has no slot table")
    at, length = header
    if length < 4:
        raise ValueError("truncated material list")
    count = struct.unpack_from('<I', data, at)[0]
    if count > (length - 4) // 4:
        raise ValueError("truncated material references")
    slots = []
    for i in range(count):
        ref = struct.unpack_from('<i', data, at + 4 + 4 * i)[0]
        if ref == -1:
            record = next(records, None)
            if record is None:
                raise ValueError("missing material record")
            slots.append(record)
        elif 0 <= ref < i:
            slots.append(slots[ref])
        else:
            raise ValueError("invalid material reference")
    return slots


def material_colours(data, start, size):
    """Every material's own colour, in order.

    A material does not have to have a texture. Plenty of the world is plain
    coloured surfaces - kerbs, railings, painted concrete, road markings - and
    the colour lives in the material itself, four bytes straight after its
    flags. Ignoring it turns every one of them white.
    """
    out = []
    body, body_size = find(data, start, start + size, MATERIAL_LIST)
    if body is None:
        return out

    for mbody, msize in material_slots(data, start, size):
        colour = (255, 255, 255, 255)
        sbody, _ssize = find(data, mbody, mbody + msize, STRUCT)
        if sbody is not None and sbody + 8 <= len(data):
            colour = tuple(data[sbody + 4:sbody + 8])
        out.append(colour)
    return out


def texture_names(data, start, size):
    """Every material's texture name, in order."""
    names = []
    body, body_size = find(data, start, start + size, MATERIAL_LIST)
    if body is None:
        return names

    for mbody, msize in material_slots(data, start, size):
        tex_body, tex_size = find(data, mbody, mbody + msize, TEXTURE)
        if tex_body is None:
            names.append(None)
            continue
        # The first string inside a Texture is its name; the second is the mask.
        name = None
        for skind, sbody, ssize in sections(data, tex_body, tex_body + tex_size):
            if skind == STRING:
                name = data[sbody:sbody + ssize].split(b"\0")[0].decode("latin-1")
                break
        names.append(name)
    return names


def split_meshes(data, start, size, vertex_count):
    """Which faces belong to which material, from the Bin Mesh extension.

    A triangle carries a material index of its own, and for stock San Andreas
    that is what says which texture it wears. But a model that has been through
    a modern exporter usually has those fields left at zero, and the real
    grouping lives here instead - in the split list the engine actually draws
    from. Reading only the triangles gives such a model one material for the
    whole thing: a stadium of 7,555 triangles wearing a single stone texture.

        uint32 flags        0 for lists of triangles, 1 for strips
        uint32 numSplits
        uint32 totalIndices
        per split:
            uint32 numIndices
            uint32 materialIndex
            uint32 indices[numIndices]

    Returns None when the model has no such list, so the triangles can speak
    for themselves.
    """
    ext, ext_size = find(data, start, start + size, EXTENSION)
    if ext is None:
        return None

    body, body_size = find(data, ext, ext + ext_size, BIN_MESH)
    if body is None:
        return None

    try:
        flags, splits, _total = struct.unpack_from("<III", data, body)
    except struct.error:
        return None

    groups = {}
    at = body + 12
    for _ in range(splits):
        if at + 8 > len(data):
            break
        count, material = struct.unpack_from("<II", data, at)
        at += 8
        if at + count * 4 > len(data):
            break

        indices = struct.unpack_from("<%dI" % count, data, at)
        at += count * 4

        faces = groups.setdefault(material, [])
        if flags == 1:
            # A strip: every run of three is a triangle, and the winding
            # alternates. Repeated indices are the joins between strips and
            # collapse to nothing, so they are dropped.
            for i in range(len(indices) - 2):
                a, b, c = indices[i], indices[i + 1], indices[i + 2]
                if a == b or b == c or a == c:
                    continue
                faces.append((a, c, b) if i % 2 else (a, b, c))
        else:
            for i in range(0, len(indices) - 2, 3):
                faces.append((indices[i], indices[i + 1], indices[i + 2]))

    # Nonsense indices mean we have misread it; better to fall back.
    for faces in groups.values():
        for face in faces:
            if max(face) >= vertex_count:
                return None

    return {m: f for m, f in groups.items() if f} or None


def parse_geometry(data, start, size):
    """Vertices, texture coordinates, and triangles grouped by material."""
    sbody, _ssize = find(data, start, start + size, STRUCT)
    if sbody is None:
        return None

    flags, num_tris, num_verts, _num_morphs = struct.unpack_from("<IIII", data, sbody)
    at = sbody + 16

    # RenderWare 3.4 and later put the ambient/diffuse/specular floats here.
    _lib = struct.unpack_from("<I", data, start - 4)[0]
    if ((_lib >> 16) & 0xFFFF) < 0x1003:
        at += 12

    prelit = []
    if flags & FLAG_PRELIT:
        prelit = [tuple(data[at + i * 4:at + i * 4 + 4])
                  for i in range(num_verts)]
        at += num_verts * 4

    uvs = []
    uv_sets = (flags & 0xFF0000) >> 16
    if uv_sets == 0:
        uv_sets = 2 if flags & FLAG_TEXTURED2 else (1 if flags & FLAG_TEXTURED else 0)
    if uv_sets:
        for i in range(num_verts):
            u, v = struct.unpack_from("<ff", data, at + i * 8)
            uvs.append((u, v))
        at += num_verts * 8 * uv_sets

    by_material = {}
    for i in range(num_tris):
        b, a, mat, c = struct.unpack_from("<HHHH", data, at + i * 8)
        by_material.setdefault(mat, []).append((a, b, c))
    at += num_tris * 8

    # Morph target: bounding sphere, then the flags for what follows.
    at += 16
    has_verts, has_normals = struct.unpack_from("<II", data, at)
    at += 8

    vertices = []
    if has_verts:
        for i in range(num_verts):
            vertices.append(struct.unpack_from("<fff", data, at + i * 12))
        at += num_verts * 12

    normals = []
    if has_normals and flags & FLAG_NORMALS:
        for i in range(num_verts):
            normals.append(struct.unpack_from("<fff", data, at + i * 12))

    # The split list wins where there is one: a triangle's own material index is
    # frequently left at zero by exporters, and trusting it paints the whole
    # model in one texture.
    splits = split_meshes(data, start, size, num_verts)
    if splits:
        by_material = splits

    return {"vertices": vertices, "uvs": uvs, "normals": normals,
            "groups": by_material, "prelit": prelit}


def read_dff(data):
    """Every mesh in a model, each with the texture its faces use."""
    out = []
    clump, clump_size = find(data, 0, len(data), CLUMP)
    if clump is None:
        return out

    glist, glist_size = find(data, clump, clump + clump_size, GEOMETRY_LIST)
    if glist is None:
        return out

    for kind, gbody, gsize in sections(data, glist, glist + glist_size):
        if kind != GEOMETRY:
            continue
        mesh = parse_geometry(data, gbody, gsize)
        if not mesh or not mesh["vertices"]:
            continue
        mesh["textures"] = texture_names(data, gbody, gsize)
        mesh["colours"] = material_colours(data, gbody, gsize)
        out.append(mesh)
    return out


def main():
    path = sys.argv[1]
    meshes = read_dff(open(path, "rb").read())
    print(f"  {path}: {len(meshes)} mesh(es)")
    for i, m in enumerate(meshes):
        tris = sum(len(v) for v in m["groups"].values())
        print(f"    mesh {i}: {len(m['vertices'])} vertices, {tris} triangles, "
              f"{len(m['uvs'])} uvs, {len(m['normals'])} normals")
        for mat, faces in sorted(m["groups"].items()):
            name = m["textures"][mat] if mat < len(m["textures"]) else None
            print(f"      material {mat}: {len(faces)} faces, texture {name!r}")


if __name__ == "__main__":
    main()
