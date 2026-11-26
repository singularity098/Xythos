# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098
"""Renders a pedal photo in Blender, in the look of the Krea-2 renders (docs/PedalImagePrompts.md): top-down, a dark
enclosure with a metal rim, a neon glyph, an empty top for the knobs and an LED recess (both drawn by the UI,
src/ui/PedalFaces.cpp), a stomp footswitch and two cables, on a dark floor in coloured stage light.

    python3 tools/blender/render_pedal.py <typeId> [--out DIR] [--samples N] [--preview]

Runs with Blender's Python module (pip install bpy; 5.x) or inside Blender (blender -b -P render_pedal.py -- <typeId>).
Writes <typeId>.png (the render, a 1:2 portrait wider than the crop) and <typeId>.json: the enclosure's box for
tools/prepare_block_image.py and the face (knobs, LED, footswitch) as fractions of the cropped image, computed from
the geometry, ready for PedalFaces.cpp. Each pedal's glyph, colours and layout are in PEDALS below.
"""
import argparse
import json
import math
import os
import sys

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Vector

# Units: 1 Blender unit = 1 cm. The pedal lies on the floor (z = 0), long axis along y, its top towards +y.
W, L, H = 5.7, 12.8, 3.0      # enclosure: width, length, height
CORNER = 0.75                 # corner radius of the outline
RIM = 0.22                    # metal rim around the black face plate
FRAME_H = 15.0                # the render's height at the floor, in cm (wider than the crop)
RES = (1100, 2200)


def hex_rgb(h):
    """sRGB hex -> linear RGB"""
    c = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in c)


# ---- glyphs: lists of polylines in a unit box (-1..1), drawn as neon tubes --------------------------------------


def circle(cx, cy, r, n=32):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def rounded_rect(cx, cy, w, h, r, n=8):
    pts = []
    for (x, y, a0) in ((w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90), (-w / 2 + r, -h / 2 + r, 180),
                       (w / 2 - r, -h / 2 + r, 270)):
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            pts.append((cx + x + r * math.cos(a), cy + y + r * math.sin(a)))
    return pts


def glyph_humeliminator():
    """A single-coil pickup (a long bar with six pole pieces) above a jagged hum wave that turns into a calm line"""
    closed, open_ = [], []
    closed.append(rounded_rect(0, 0.42, 1.5, 0.42, 0.21))
    for i in range(6):
        closed.append(circle(-0.55 + 0.22 * i, 0.42, 0.065, 20))
    wave = []
    for i in range(8):  # zigzag on the left, shrinking as it goes, then a calm line
        amp = 0.27 * (1 - i / 8) ** 0.9
        wave.append((-0.7 + i * 0.1, -0.38 + (-amp if i % 2 == 0 else amp)))
    wave.append((0.16, -0.38))
    wave.append((0.74, -0.38))
    open_.append(wave)
    return closed, open_


PEDALS = {
    "humeliminator": {
        "glyph": glyph_humeliminator,
        "glyph_color": "3fb4ff",    # electric blue
        "glyph_size": 2.35,         # half-width of the glyph's unit box, cm
        "glyph_y": 0.35,
        "ring": True,               # a thin neon ring around the glyph
        "knobs": ["Amount", "Adapt"],
        "lights": ("19c6d6", "e0389b"),  # left and right rim light: cyan and magenta
    },
}

# ---- scene helpers ------------------------------------------------------------------------------------------------


def material(name, color=(0.5, 0.5, 0.5), metallic=0.0, roughness=0.5, emission=None, strength=0.0, coat=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*color, 1)
    b.inputs["Metallic"].default_value = metallic
    b.inputs["Roughness"].default_value = roughness
    if coat:
        b.inputs["Coat Weight"].default_value = coat
    if emission:
        b.inputs["Emission Color"].default_value = (*emission, 1)
        b.inputs["Emission Strength"].default_value = strength
    return m


def add_noise_bump(m, scale, strength, stretch=(1, 1, 1), rough_range=None, detail=8.0):
    """Fine surface texture: noise into a bump (and optionally into the roughness), stretched for brushed metal"""
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    coord = nt.nodes.new("ShaderNodeTexCoord")
    mapping = nt.nodes.new("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = stretch
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = scale
    noise.inputs["Detail"].default_value = detail
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = strength
    bump.inputs["Distance"].default_value = 0.01
    nt.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
    nt.links.new(mapping.outputs["Vector"], noise.inputs["Vector"])
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], b.inputs["Normal"])
    if rough_range:
        ramp = nt.nodes.new("ShaderNodeMapRange")
        ramp.inputs["To Min"].default_value = rough_range[0]
        ramp.inputs["To Max"].default_value = rough_range[1]
        nt.links.new(noise.outputs["Fac"], ramp.inputs["Value"])
        nt.links.new(ramp.outputs["Result"], b.inputs["Roughness"])


def floor_albedo(m):
    """Concrete: blotchy albedo from two scales of noise"""
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 0.35
    noise.inputs["Detail"].default_value = 10
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.015, 0.015, 0.017, 1)
    ramp.color_ramp.elements[1].color = (0.09, 0.09, 0.095, 1)
    nt.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], b.inputs["Base Color"])


def softbox(name, color, strength, loc, size, gradient=False):
    """An emitting card seen only in reflections: what the chrome and the rim mirror"""
    bpy.ops.mesh.primitive_plane_add(size=1, location=loc)
    ob = bpy.context.object
    ob.name = name
    ob.scale = (size[0], size[1], 1)
    ob.rotation_euler = (math.pi, 0, 0)  # facing down
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.remove(nt.nodes["Principled BSDF"])
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*color, 1)
    em.inputs["Strength"].default_value = strength
    if gradient:  # brighter towards one end, so the reflections aren't flat
        coord = nt.nodes.new("ShaderNodeTexCoord")
        grad = nt.nodes.new("ShaderNodeTexGradient")
        mul = nt.nodes.new("ShaderNodeMath")
        mul.operation = "MULTIPLY"
        mul.inputs[1].default_value = strength
        nt.links.new(coord.outputs["Generated"], grad.inputs["Vector"])
        nt.links.new(grad.outputs["Fac"], mul.inputs[0])
        nt.links.new(mul.outputs[0], em.inputs["Strength"])
    nt.links.new(em.outputs["Emission"], nt.nodes["Material Output"].inputs["Surface"])
    ob.data.materials.append(m)
    ob.visible_camera = False
    ob.visible_diffuse = False
    ob.visible_shadow = False
    return ob


def curve_object(name, polylines, cyclic, z, bevel=0.0, extrude=0.0, fill=False, mat=None, resolution=4):
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "2D" if fill else "3D"
    if fill:
        cu.fill_mode = "BOTH"
    cu.extrude = extrude
    cu.bevel_depth = bevel
    cu.bevel_resolution = resolution
    for pts in polylines:
        sp = cu.splines.new("POLY")
        sp.points.add(len(pts) - 1)
        for p, (x, y) in zip(sp.points, pts):
            p.co = (x, y, 0, 1)
        sp.use_cyclic_u = cyclic
    ob = bpy.data.objects.new(name, cu)
    ob.location.z = z
    bpy.context.collection.objects.link(ob)
    if mat:
        cu.materials.append(mat)
    return ob


def cylinder(name, r, depth, loc, mat, rot=(0, 0, 0), verts=64, bevel=0.0, smooth=True):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=depth, location=loc, rotation=rot, vertices=verts)
    ob = bpy.context.object
    ob.name = name
    ob.data.materials.append(mat)
    if bevel:
        mod = ob.modifiers.new("bevel", "BEVEL")
        mod.width = bevel
        mod.segments = 4
        mod.limit_method = "ANGLE"
    if smooth:
        bpy.ops.object.shade_auto_smooth(angle=math.radians(40))
    return ob


def torus(name, major, minor, loc, mat):
    bpy.ops.mesh.primitive_torus_add(major_radius=major, minor_radius=minor, location=loc, major_segments=64,
                                     minor_segments=16)
    ob = bpy.context.object
    ob.name = name
    ob.data.materials.append(mat)
    bpy.ops.object.shade_smooth()
    return ob


def spot(name, color, energy, loc, target, size_deg=60, blend=0.8, radius=1.0):
    data = bpy.data.lights.new(name, "SPOT")
    data.color = color
    data.energy = energy
    data.spot_size = math.radians(size_deg)
    data.spot_blend = blend
    data.shadow_soft_size = radius
    ob = bpy.data.objects.new(name, data)
    ob.location = loc
    bpy.context.collection.objects.link(ob)
    ob.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    return ob


def area(name, color, energy, loc, target, size):
    data = bpy.data.lights.new(name, "AREA")
    data.color = color
    data.energy = energy
    data.size = size
    ob = bpy.data.objects.new(name, data)
    ob.location = loc
    bpy.context.collection.objects.link(ob)
    ob.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    return ob


# ---- the pedal ----------------------------------------------------------------------------------------------------


def build(spec):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    glow = hex_rgb(spec["glyph_color"])

    alu = material("brushed aluminium", (0.62, 0.63, 0.65), metallic=1.0, roughness=0.3)
    add_noise_bump(alu, 60, 0.08, stretch=(1, 40, 1), rough_range=(0.22, 0.38))
    face = material("black face", (0.004, 0.004, 0.005), roughness=0.6)
    face.node_tree.nodes["Principled BSDF"].inputs["Specular IOR Level"].default_value = 0.25
    add_noise_bump(face, 220, 0.12, rough_range=(0.55, 0.75), detail=4)
    chrome = material("chrome", (0.85, 0.86, 0.88), metallic=1.0, roughness=0.12)
    dark_metal = material("dark metal", (0.05, 0.05, 0.055), metallic=1.0, roughness=0.3)
    hole = material("hole", (0.0, 0.0, 0.0), roughness=0.9)
    rubber = material("rubber", (0.012, 0.012, 0.012), roughness=0.6)
    neon = material("neon", glow, emission=glow, strength=2.2)
    neon_dim = material("neon ring", glow, emission=glow, strength=1.1)
    floor = material("floor", (0.07, 0.07, 0.075), roughness=0.85)
    add_noise_bump(floor, 2.5, 0.6, rough_range=(0.55, 0.95))
    floor_albedo(floor)

    # enclosure: a rounded outline extruded, its top edge rounded by the curve's bevel
    bevel = 0.18
    outline = rounded_rect(0, 0, W - 2 * bevel, L - 2 * bevel, CORNER - bevel, 12)
    curve_object("enclosure", [outline], True, (H - bevel) / 2, bevel=bevel, extrude=(H - bevel) / 2, fill=True,
                 mat=alu, resolution=5)
    plate = rounded_rect(0, 0, W - 2 * RIM, L - 2 * RIM, CORNER - RIM * 0.8, 12)
    curve_object("face plate", [plate], True, H + 0.01, extrude=0.01, fill=True, mat=face)
    top = H + 0.02

    # glyph: neon tubes on the face plate
    s, gy = spec["glyph_size"], spec["glyph_y"]
    closed, open_ = spec["glyph"]()
    tf = lambda pts: [(x * s, gy + y * s) for x, y in pts]
    curve_object("glyph", [tf(p) for p in closed], True, top + 0.035, bevel=0.055, mat=neon)
    curve_object("glyph open", [tf(p) for p in open_], False, top + 0.035, bevel=0.055, mat=neon)
    if spec.get("ring"):
        curve_object("ring", [circle(0, gy, s * 1.02, 128)], True, top + 0.02, bevel=0.025, mat=neon_dim)

    # LED recess (the UI draws the LED over it) and the footswitch
    led_y, sw_y = -3.15, -4.75
    torus("led bezel", 0.24, 0.05, (0, led_y, top + 0.02), dark_metal)
    cylinder("led hole", 0.22, 0.04, (0, led_y, top), hole, verts=48)
    cylinder("switch nut", 0.78, 0.16, (0, sw_y, top + 0.08), chrome, verts=6, bevel=0.03, smooth=False)
    cylinder("switch washer", 0.85, 0.04, (0, sw_y, top + 0.02), chrome, bevel=0.01)
    cylinder("switch bushing", 0.58, 0.3, (0, sw_y, top + 0.3), chrome)
    cylinder("switch cap", 0.5, 0.75, (0, sw_y, top + 0.75), chrome, bevel=0.1)

    # jacks and cables on both sides, a little above the middle
    jack_y, jack_z = 0.9, H * 0.55
    for side in (-1, 1):
        x = side * W / 2
        rot = (0, math.pi / 2, 0)
        cylinder("jack nut", 0.5, 0.18, (x + side * 0.09, jack_y, jack_z), chrome, rot=rot, verts=6, bevel=0.02,
                 smooth=False)
        cylinder("plug", 0.34, 1.1, (x + side * 0.72, jack_y, jack_z), chrome, rot=rot, bevel=0.05)
        cylinder("boot", 0.36, 1.2, (x + side * 1.85, jack_y, jack_z), rubber, rot=rot, bevel=0.08)
        cylinder("cable", 0.24, 4.0, (x + side * 4.3, jack_y, jack_z * 0.8), rubber, rot=(0, math.pi / 2 - side * 0.1, 0))

    # floor
    bpy.ops.mesh.primitive_plane_add(size=80, location=(0, 0, 0))
    bpy.context.object.data.materials.append(floor)

    # lights: a soft key from above the top for the metal, coloured rim lights low from the sides
    left, right = spec["lights"]
    area("key", (1.0, 0.92, 0.85), 2200, (0, 14, 32), (0, -1, 0), 12)
    spot("left", hex_rgb(left), 9000, (-30, 6, 9), (0, 0, 1), size_deg=70)
    spot("right", hex_rgb(right), 9000, (30, -6, 9), (0, 0, 1), size_deg=70)
    softbox("softbox top", (1.0, 0.95, 0.9), 2.4, (0, 8, 30), (30, 22), gradient=True)
    softbox("softbox left", hex_rgb(left), 3.0, (-14, 0, 12), (6, 40))
    softbox("softbox right", hex_rgb(right), 3.0, (14, 0, 12), (6, 40))
    world = bpy.data.worlds.new("world")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.004, 0.004, 0.007, 1)
    scene.world = world

    # camera: straight down, a long lens so the sides barely show
    cam = bpy.data.cameras.new("camera")
    cam.lens = 100
    cam.sensor_fit = "VERTICAL"
    cam.sensor_height = 24
    dist = FRAME_H / (cam.sensor_height / cam.lens)
    cam_ob = bpy.data.objects.new("camera", cam)
    cam_ob.location = (0, 0, dist)
    bpy.context.collection.objects.link(cam_ob)
    scene.camera = cam_ob

    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = RES
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "Khronos PBR Neutral"  # keeps the neon saturated (AgX paled it)
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_depth = "16"
    return scene, cam_ob, top, led_y, sw_y


def to_frame(scene, cam, p):
    """A world point -> fractions of the render (x right, y down)"""
    v = world_to_camera_view(scene, cam, Vector(p))
    return v.x, 1 - v.y


def crop_box(width, height, box):
    """The same crop as tools/prepare_block_image.py"""
    x0, y0, x1, y1 = box[0] * width, box[1] * height, box[2] * width, box[3] * height
    ch = min(max((y1 - y0) * 1.06, (x1 - x0) * 1.16 * 2), height, width * 2)
    cw = ch / 2
    left = min(max((x0 + x1) / 2 - cw / 2, 0), width - cw)
    top = min(max((y0 + y1) / 2 - ch / 2, 0), height - ch)
    return left, top, cw, ch


def face_json(scene, cam, spec, top, led_y, sw_y):
    wpx, hpx = RES
    a = to_frame(scene, cam, (-W / 2, L / 2, H))
    b = to_frame(scene, cam, (W / 2, -L / 2, H))
    box = [a[0], a[1], b[0], b[1]]
    left, ctop, cw, ch = crop_box(wpx, hpx, box)

    def crop_pt(p):
        x, y = to_frame(scene, cam, p)
        return round((x * wpx - left) / cw, 3), round((y * hpx - ctop) / ch, 3)

    def crop_r(p, r):
        x0, _ = to_frame(scene, cam, p)
        x1, _ = to_frame(scene, cam, (p[0] + r, p[1], p[2]))
        return round((x1 - x0) * wpx / cw, 3)

    # knobs: centred in the blank area between the enclosure's top and the glyph's ring
    glyph_top = spec["glyph_y"] + spec["glyph_size"] * 1.05
    plate_top = L / 2 - RIM
    knobs, names = [], spec["knobs"]
    kr = 0.75 if len(names) < 3 else 0.62
    ky = (glyph_top + plate_top) / 2
    xs = {1: [0], 2: [-1.0, 1.0], 3: [-1.15, 0, 1.15]}[len(names)]
    for i, (name, x) in enumerate(zip(names, xs)):
        y = ky + (0.3 if len(names) == 3 and i == 1 else 0)
        p = (x, y, top)
        knobs.append({"param": name, "x": crop_pt(p)[0], "y": crop_pt(p)[1], "radius": crop_r(p, kr)})
    led = (0, led_y, top)
    sw = (0, sw_y, top + 1.1)
    return {
        "box": [round(v, 4) for v in box],
        "knobs": knobs,
        "led": {"x": crop_pt(led)[0], "y": crop_pt(led)[1], "radius": crop_r(led, (0.24 + 0.05) * 1.3 / 1.42)}  # the drawn bezel (1.42x) covers the recess,
        "footswitch": {"x": crop_pt(sw)[0], "y": crop_pt(sw)[1], "radius": crop_r(sw, 0.95)},
        "color": spec["glyph_color"],
    }


def bloom(path, colors):
    """The neon's glow and a faint haze, as a camera would see them (Cycles has no bloom)"""
    import numpy as np
    from PIL import Image, ImageFilter

    img = Image.open(path).convert("RGB")
    a = np.asarray(img).astype(np.float32) / 255
    lum = a.max(axis=2)
    bright = np.clip((lum - 0.55) / 0.45, 0, 1)[..., None] * a
    src = Image.fromarray((bright * 255).astype(np.uint8))
    glow = np.zeros_like(a)
    for radius, weight in ((6, 0.5), (24, 0.45), (80, 0.35)):
        glow += np.asarray(src.filter(ImageFilter.GaussianBlur(radius))).astype(np.float32) / 255 * weight
    # haze: soft fractal smoke, tinted by the side lights, thicker towards the edges
    rng = np.random.default_rng(7)
    smoke = np.zeros(lum.shape, np.float32)
    for scale, weight in ((8, 0.5), (24, 0.3), (64, 0.2)):
        n = rng.random((max(2, lum.shape[0] // (scale * 4)), max(2, lum.shape[1] // (scale * 4))))
        n = Image.fromarray((n * 255).astype(np.uint8)).resize(img.size, Image.BICUBIC)
        smoke += np.asarray(n.filter(ImageFilter.GaussianBlur(scale))).astype(np.float32) / 255 * weight
    smoke = np.clip((smoke - 0.35) * 2.2, 0, 1) ** 1.5
    x = np.linspace(0, 1, lum.shape[1])[None, :]
    edge = 0.35 + 0.65 * np.abs(x - 0.5) * 2
    c0, c1 = (np.array([int(c[i:i + 2], 16) / 255 for i in (0, 2, 4)]) for c in colors)
    tint = c0[None, None, :] * (1 - x[..., None]) + c1[None, None, :] * x[..., None]
    glow += (smoke * edge)[..., None] * tint * 0.16 * (0.25 + 0.75 * (lum < 0.08))[..., None]
    out = 1 - (1 - a) * (1 - np.clip(glow, 0, 1))  # screen
    Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("type_id", choices=sorted(PEDALS))
    parser.add_argument("--out", default=".")
    parser.add_argument("--samples", type=int, default=128)
    parser.add_argument("--preview", action="store_true", help="half size, few samples")
    args = parser.parse_args(argv)

    spec = PEDALS[args.type_id]
    scene, cam, top, led_y, sw_y = build(spec)
    scene.cycles.samples = 24 if args.preview else args.samples
    if args.preview:
        scene.render.resolution_percentage = 50
    os.makedirs(args.out, exist_ok=True)
    png = os.path.abspath(os.path.join(args.out, f"{args.type_id}.png"))
    scene.render.filepath = png
    bpy.ops.render.render(write_still=True)
    bloom(png, spec["lights"])
    face = face_json(scene, cam, spec, top, led_y, sw_y)
    with open(os.path.join(args.out, f"{args.type_id}.json"), "w") as f:
        json.dump(face, f, indent=2)
    print(json.dumps(face, indent=2))


if __name__ == "__main__":
    main()
