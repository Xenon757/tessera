#!/usr/bin/env python3
"""Writes small binary FBX files with known content, plus the vertex positions a
correct loader must produce, for tests/test_loaders.cpp.

The scene exercises what's easy to get wrong: a parent/child model hierarchy,
pre-rotation, a non-default rotation order, a geometric (object-offset) transform,
non-uniform scale, a Z-up axis system, quads and an n-gon, normals present and
absent. It's written twice: FBX 7.4 with raw arrays (32-bit node headers) and FBX
7.5 with zlib-compressed arrays (64-bit node headers). The expected positions are
computed here, independently of the C++ loader, using the FBX SDK's transform
formula.

Usage: make_fbx.py <outdir>   -> scene_7400.fbx, scene_7500z.fbx, expected.txt"""
import math
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------- FBX writer


def prop(value):
    """One property: (type code, payload). Python types pick the FBX type."""
    if isinstance(value, tuple) and value[0] == "L":
        return b"L" + struct.pack("<q", value[1])
    if isinstance(value, bool):
        return b"C" + struct.pack("<B", value)
    if isinstance(value, int):
        return b"I" + struct.pack("<i", value)
    if isinstance(value, float):
        return b"D" + struct.pack("<d", value)
    if isinstance(value, str):
        data = value.encode()
        return b"S" + struct.pack("<I", len(data)) + data
    kind, items, compress = value  # arrays: ("d" | "i", [values], compress?)
    raw = struct.pack("<%d%s" % (len(items), "d" if kind == "d" else "i"), *items)
    data = zlib.compress(raw) if compress else raw
    return kind.encode() + struct.pack("<III", len(items), 1 if compress else 0, len(data)) + data


def node_bytes(name, props, children, offset, wide):
    """A node record starting at absolute file offset `offset`."""
    head_size = (8 * 3 if wide else 4 * 3) + 1 + len(name)
    props_data = b"".join(prop(p) for p in props)
    body = b""
    pos = offset + head_size + len(props_data)
    for child in children:
        data = node_bytes(*child, pos, wide)
        body += data
        pos += len(data)
    if children:
        body += b"\0" * (25 if wide else 13)  # null record ends the child list
    end = offset + head_size + len(props_data) + len(body)
    fmt = "<QQQ" if wide else "<III"
    return struct.pack(fmt, end, len(props), len(props_data)) + bytes([len(name)]) + name.encode() + props_data + body


def write_fbx(path, version, roots):
    wide = version >= 7500
    data = b"Kaydara FBX Binary  \0\x1a\0" + struct.pack("<I", version)
    for root in roots:
        data += node_bytes(*root, len(data), wide)
    data += b"\0" * (25 if wide else 13)
    with open(path, "wb") as f:
        f.write(data)


# ---------------------------------------------------------------- the scene

def p70(*entries):
    """Properties70 with P records: (name, x, y, z) or (name, int)."""
    records = []
    for e in entries:
        if len(e) == 2:
            records.append(("P", [e[0], "int", "Integer", "", e[1]], []))
        else:
            records.append(("P", [e[0], "Vector3D", "Vector", "", float(e[1]), float(e[2]), float(e[3])], []))
    return ("Properties70", [], records)


CUBE_POINTS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1), (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
CUBE_QUADS = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (2, 3, 7, 6), (1, 2, 6, 5), (0, 4, 7, 3)]  # CCW from outside
NGON_POINTS = [(math.cos(a) * 2, math.sin(a) * 2, 0.5) for a in [i * 2 * math.pi / 6 for i in range(6)]]
NGON = [(0, 1, 2, 3, 4, 5)]  # one hexagon, fan-triangulated by the loader

PARENT = dict(T=(1, 2, 3), R=(10, 20, 30), order=0, S=(1, 2, 1))
CHILD = dict(T=(0, 0, 5), pre=(0, 0, 90), R=(0, 45, 0), order=4, S=(2, 2, 2), GT=(1, 0, 0))


def polygon_index(polys):
    out = []
    for poly in polys:
        out += list(poly[:-1]) + [-poly[-1] - 1]  # a negative index ends each polygon
    return out


def scene(compress):
    cube_normals = []
    for quad in CUBE_QUADS:
        a, b, c = (CUBE_POINTS[i] for i in quad[:3])
        u = [b[k] - a[k] for k in range(3)]
        v = [c[k] - a[k] for k in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(x * x for x in n))
        cube_normals += [x / length for x in n] * 4
    geometry_cube = ("Geometry", [("L", 100), "Cube\0\x01Geometry", "Mesh"], [
        ("Vertices", [("d", [float(c) for p in CUBE_POINTS for c in p], compress)], []),
        ("PolygonVertexIndex", [("i", polygon_index(CUBE_QUADS), compress)], []),
        ("LayerElementNormal", [0], [
            ("MappingInformationType", ["ByPolygonVertex"], []),
            ("ReferenceInformationType", ["Direct"], []),
            ("Normals", [("d", cube_normals, compress)], []),
        ]),
        ("LayerElementUV", [0], [
            ("MappingInformationType", ["ByPolygonVertex"], []),
            ("ReferenceInformationType", ["IndexToDirect"], []),
            ("UV", [("d", [0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0], compress)], []),
            ("UVIndex", [("i", [0, 1, 2, 3] * 6, compress)], []),
        ]),
    ])
    geometry_ngon = ("Geometry", [("L", 101), "Hex\0\x01Geometry", "Mesh"], [
        ("Vertices", [("d", [float(c) for p in NGON_POINTS for c in p], compress)], []),
        ("PolygonVertexIndex", [("i", polygon_index(NGON), compress)], []),
    ])
    parent = ("Model", [("L", 200), "Parent\0\x01Model", "Mesh"], [p70(
        ("Lcl Translation", *PARENT["T"]), ("Lcl Rotation", *PARENT["R"]), ("Lcl Scaling", *PARENT["S"]),
        ("RotationOrder", PARENT["order"]))])
    child = ("Model", [("L", 300), "Child\0\x01Model", "Mesh"], [p70(
        ("Lcl Translation", *CHILD["T"]), ("PreRotation", *CHILD["pre"]), ("Lcl Rotation", *CHILD["R"]),
        ("Lcl Scaling", *CHILD["S"]), ("RotationOrder", CHILD["order"]), ("GeometricTranslation", *CHILD["GT"]))])
    return [
        ("GlobalSettings", [], [p70(("UpAxis", 2), ("UpAxisSign", 1), ("FrontAxis", 1), ("FrontAxisSign", -1),
                                    ("CoordAxis", 0), ("CoordAxisSign", 1))]),
        ("Objects", [], [geometry_cube, geometry_ngon, parent, child]),
        ("Connections", [], [
            ("C", ["OO", ("L", 200), ("L", 0)], []),
            ("C", ["OO", ("L", 300), ("L", 200)], []),
            ("C", ["OO", ("L", 100), ("L", 300)], []),
            ("C", ["OO", ("L", 101), ("L", 200)], []),
        ]),
    ]


# ---------------------------------------------------------------- expected output (4x4 matrices as row lists)

def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def translate(t):
    return [[1, 0, 0, t[0]], [0, 1, 0, t[1]], [0, 0, 1, t[2]], [0, 0, 0, 1]]


def scale(s):
    return [[s[0], 0, 0, 0], [0, s[1], 0, 0], [0, 0, s[2], 0], [0, 0, 0, 1]]


def rot(axis, degrees):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    if axis == "x":
        return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]]
    if axis == "y":
        return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]
    return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def euler(r, order):
    """FBX order enum 0..5 = XYZ XZY YZX YXZ ZXY ZYX; the first letter is applied first."""
    names = ["xyz", "xzy", "yzx", "yxz", "zxy", "zyx"][order]
    m = [[1 if i == j else 0 for j in range(4)] for i in range(4)]
    for axis in names:  # applied first = rightmost in the product
        m = mul(rot(axis, r["xyz".index(axis)]), m)
    return m


def local(n):
    return mul(mul(mul(translate(n["T"]), euler(n.get("pre", (0, 0, 0)), 0)), euler(n["R"], n["order"])), scale(n["S"]))


def apply(m, p):
    return [sum(m[i][k] * (list(p) + [1])[k] for k in range(4)) for i in range(3)]


def triangles(points, polys, matrix):
    out = []
    for poly in polys:
        for i in range(1, len(poly) - 1):
            for k in (poly[0], poly[i], poly[i + 1]):
                x, y, z = apply(matrix, points[k])
                out.append((x, z, -y))  # Z-up file -> our Y-up: right +X, up +Z, front -Y
    return out


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    write_fbx(os.path.join(outdir, "scene_7400.fbx"), 7400, scene(False))
    write_fbx(os.path.join(outdir, "scene_7500z.fbx"), 7500, scene(True))
    parent_world = local(PARENT)
    child_world = mul(mul(parent_world, local(CHILD)), translate(CHILD["GT"]))
    expected = triangles(CUBE_POINTS, CUBE_QUADS, child_world) + triangles(NGON_POINTS, NGON, parent_world)
    with open(os.path.join(outdir, "expected.txt"), "w") as f:
        for p in expected:
            f.write("%.9g %.9g %.9g\n" % p)


if __name__ == "__main__":
    main()
