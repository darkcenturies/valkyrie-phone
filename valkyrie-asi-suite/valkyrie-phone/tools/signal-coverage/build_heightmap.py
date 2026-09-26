"""Builds a heightmap of the map from 500 x 500 world tiles in glTF (.glb).

The tiles are named <x>_<y>.glb, sit in world coordinates with Z up, and may
use KHR_mesh_quantization. Every triangle is sampled into 25-unit cells; each
cell keeps the highest point that falls in it. Cells no tile touches stay at
-1e9 (open sea, or ground the export left out).

    python build_heightmap.py <tile folder> heightmap.npy
"""
import glob
import json
import os
import struct
import sys

import numpy as np

CELL = 25
X0, X1, Y0, Y1 = -5000, 19500, -12500, 16000
WIDTH, HEIGHT = (X1 - X0) // CELL, (Y1 - Y0) // CELL
TYPES = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
         5125: np.uint32, 5126: np.float32}


def rasterise(heights, points, indices):
    triangles = points[indices.reshape(-1, 3)]
    edges = np.stack([
        np.linalg.norm(triangles[:, a, :2] - triangles[:, b, :2], axis=1)
        for a, b in ((0, 1), (1, 2), (0, 2))]).max(0)
    samples = [points]
    large = triangles[edges > CELL]
    if len(large):
        steps = int(min(40, np.ceil(edges[edges > CELL].max() / CELL))) + 1
        for i in range(steps + 1):
            for j in range(steps + 1 - i):
                a, b = i / steps, j / steps
                samples.append(large[:, 0] * (1 - a - b) + large[:, 1] * a + large[:, 2] * b)
    q = np.concatenate(samples)
    ix = ((q[:, 0] - X0) // CELL).astype(int)
    iy = ((Y1 - q[:, 1]) // CELL).astype(int)
    inside = (ix >= 0) & (ix < WIDTH) & (iy >= 0) & (iy < HEIGHT)
    np.maximum.at(heights, (iy[inside], ix[inside]), q[inside, 2])


def read_tile(path, heights):
    data = open(path, "rb").read()
    length = struct.unpack_from("<I", data, 12)[0]
    gltf = json.loads(data[20:20 + length])
    binary = 20 + length + 8
    for node in gltf["nodes"]:
        if "mesh" not in node:
            continue
        scale = np.array(node.get("scale", [1, 1, 1]))
        move = np.array(node.get("translation", [0, 0, 0]))
        for primitive in gltf["meshes"][node["mesh"]]["primitives"]:
            accessor = gltf["accessors"][primitive["attributes"]["POSITION"]]
            view = gltf["bufferViews"][accessor["bufferView"]]
            kind = TYPES[accessor["componentType"]]
            size = np.dtype(kind).itemsize
            stride = view.get("byteStride", 3 * size)
            count = accessor["count"]
            start = binary + view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            rows = np.frombuffer(data, np.uint8, count=stride * (count - 1) + 3 * size,
                                 offset=start)
            rows = np.pad(rows, (0, stride * count - len(rows))).reshape(count, stride)
            points = rows[:, :3 * size].copy().view(kind).reshape(-1, 3) * scale + move
            if "indices" in primitive:
                ia = gltf["accessors"][primitive["indices"]]
                iv = gltf["bufferViews"][ia["bufferView"]]
                indices = np.frombuffer(
                    data, TYPES[ia["componentType"]], count=ia["count"],
                    offset=binary + iv.get("byteOffset", 0) + ia.get("byteOffset", 0))
                indices = indices.astype(np.int64)
            else:
                indices = np.arange(len(points) - len(points) % 3)
            rasterise(heights, points, indices)


def main(folder, output):
    heights = np.full((HEIGHT, WIDTH), -1e9, np.float32)
    tiles = glob.glob(os.path.join(folder, "*.glb"))
    for n, tile in enumerate(tiles):
        read_tile(tile, heights)
        if n % 100 == 0:
            print(f"{n}/{len(tiles)} tiles", file=sys.stderr)
    np.save(output, heights)
    print(f"{WIDTH} x {HEIGHT} cells, {(heights > -1e9).mean():.0%} covered by tiles")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "heightmap.npy")
