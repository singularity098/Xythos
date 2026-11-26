# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098
"""Renders Xythos pedals in other looks than the neon one (render_pedal.py): clean studio enclosures, engraved metal,
wood, acrylic, hammertone... An exploration tool: same geometry and top-down camera as render_pedal.py, so the face
placements (knobs, LED, footswitch) it prints still apply.

    python3 tools/blender/render_styles.py <style> <typeId> [<typeId>...] --glyphs glyphs.json [--out DIR]
                                           [--samples N] [--scale PCT]

The glyphs come from src/ui/BlockGlyphs.cpp, dumped to JSON by a small program (see docs/PedalImagePrompts.md,
"Blender renders"): {typeId: {"strokes": [[[x, y], ...], ...], "dots": [[x, y, r], ...]}} in the 124x200 pedal frame.
"""
import argparse
import json
import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_pedal as rp  # noqa: E402
from render_pedal import W, L, H, hex_rgb, material, add_noise_bump, curve_object, cylinder, torus, area, softbox  # noqa: E402,E501

# ---- styles -------------------------------------------------------------------------------------------------------
# body: the enclosure's material; plate: the top face (None: the body's own top, no rim); glyph: how the symbol is
# made (neon, print, engrave, inlay, edgelit); stage: floor and lights; corner, rim: the outline

STYLES = {
    "clean-white": dict(body="powder_white", plate=None, glyph="print", ink="1c1d22", corner=1.2, stage="studio_white",
                        switch="satin", led="flush"),
    "clean-black": dict(body="anodized_black", plate=None, glyph="engrave", ink="b9bcc4", corner=1.2,
                        stage="studio_grey", switch="black", led="flush"),
    "clean-slate": dict(body="slate", plate=None, glyph="print", ink="9aa0ff", corner=1.2, stage="studio_grey",
                        switch="black", led="flush"),
    "clean-sand": dict(body="sand", plate=None, glyph="print", ink="b5532e", corner=1.2, stage="studio_white",
                       switch="satin", led="flush"),
    "pastel": dict(body="gloss_pastel", plate=None, glyph="print", ink="ffffff", corner=1.6, stage="studio_pastel",
                   switch="satin", led="flush"),
    "raw-aluminium": dict(body="brushed", plate=None, glyph="engrave", ink="202124", corner=0.6,
                          stage="studio_grey", switch="chrome", led="bezel"),
    "hammertone": dict(body="hammertone", plate=None, glyph="print", ink="efe3c4", corner=0.5, stage="warm_wood",
                       switch="chrome", led="bezel"),
    "walnut": dict(body="brushed_dark", plate="walnut", rim=0.32, glyph="inlay", ink="d8a960", corner=0.9,
                   stage="warm_dark", switch="brass", led="bezel"),
    "acrylic": dict(body="anodized_black", plate="smoked_acrylic", rim=0.25, glyph="edgelit", corner=0.9,
                    stage="stage", switch="black", led="flush"),
    "copper": dict(body="copper", plate=None, glyph="engrave", ink="1b0f0a", corner=0.9, stage="warm_dark",
                   switch="chrome", led="bezel"),
    "bauhaus": dict(body="matte_colour", plate=None, glyph="print", ink="f4f1ea", corner=0.3, stage="studio_white",
                    switch="black", led="flush"),
    "neon": dict(body="brushed", plate="black_face", rim=0.22, glyph="neon", corner=0.75, stage="stage",
                 switch="chrome", led="bezel"),
}

# per-pedal colour for the styles that take one (pastel, bauhaus, neon, acrylic)
PEDAL_COLOURS = {
    "overdrive": ("9fe0c3", "e2a019", "3fff8a"), "delay": ("a9c8f2", "1f5fbf", "3fb4ff"),
    "chorus": ("d6b8f0", "6b3fa0", "c06bff"), "fuzz": ("f6b3a8", "d2321e", "ff4a3a"),
    "reverb": ("b9e4ef", "1e7f8c", "5fe8ff"), "compressor": ("f4dc9a", "222222", "ffc23f"),
    "phaser": ("f2b6d6", "c2286e", "ff5fb4"), "tremolo": ("c8e6a0", "3c7a2a", "b4ff3f"),
    "octave": ("f0c8a0", "d26a1e", "ff9a3f"), "wah": ("e4c4b0", "7a3a20", "ff7a3f"),
    "boost": ("f8e3a3", "e8b400", "fff04a"),
}


def pedal_colour(type_id, index):
    return PEDAL_COLOURS.get(type_id, ("cfd4dc", "3a3f4a", "7278fe"))[index]


def body_material(kind, type_id):
    if kind == "powder_white":
        m = material("powder white", (0.80, 0.80, 0.78), roughness=0.55)
        add_noise_bump(m, 400, 0.05, rough_range=(0.5, 0.6), detail=3)
    elif kind == "anodized_black":
        m = material("anodized black", (0.018, 0.018, 0.02), metallic=0.6, roughness=0.42)
        add_noise_bump(m, 500, 0.03, rough_range=(0.38, 0.46), detail=3)
    elif kind == "slate":
        m = material("slate", (0.035, 0.04, 0.05), roughness=0.6)
        add_noise_bump(m, 400, 0.04, rough_range=(0.55, 0.65), detail=3)
    elif kind == "sand":
        m = material("sand", (0.56, 0.47, 0.36), roughness=0.62)
        add_noise_bump(m, 400, 0.04, rough_range=(0.58, 0.66), detail=3)
    elif kind == "gloss_pastel":
        m = material("pastel", hex_rgb(pedal_colour(type_id, 0)), roughness=0.18, coat=0.6)
    elif kind == "matte_colour":
        m = material("bauhaus", hex_rgb(pedal_colour(type_id, 1)), roughness=0.6)
        add_noise_bump(m, 400, 0.03, detail=2)
    elif kind == "brushed":
        m = material("brushed aluminium", (0.66, 0.67, 0.69), metallic=1.0, roughness=0.28)
        add_noise_bump(m, 60, 0.08, stretch=(1, 40, 1), rough_range=(0.2, 0.36))
    elif kind == "brushed_dark":
        m = material("dark bronze", (0.12, 0.09, 0.07), metallic=1.0, roughness=0.32)
        add_noise_bump(m, 60, 0.06, stretch=(1, 40, 1), rough_range=(0.26, 0.4))
    elif kind == "copper":
        m = material("copper", (0.93, 0.55, 0.38), metallic=1.0, roughness=0.22)
        add_noise_bump(m, 60, 0.05, stretch=(40, 1, 1), rough_range=(0.16, 0.3))
    elif kind == "hammertone":
        m = material("hammertone", (0.10, 0.20, 0.17), metallic=0.75, roughness=0.35)
        nt = m.node_tree
        b = nt.nodes["Principled BSDF"]
        vor = nt.nodes.new("ShaderNodeTexVoronoi")
        vor.inputs["Scale"].default_value = 14.0
        vor.feature = "SMOOTH_F1"
        bump = nt.nodes.new("ShaderNodeBump")
        bump.inputs["Strength"].default_value = 0.6
        bump.inputs["Distance"].default_value = 0.04
        nt.links.new(vor.outputs["Distance"], bump.inputs["Height"])
        nt.links.new(bump.outputs["Normal"], b.inputs["Normal"])
    else:
        raise ValueError(kind)
    return m


def plate_material(kind):
    if kind == "black_face":
        m = material("black face", (0.004, 0.004, 0.005), roughness=0.6)
        add_noise_bump(m, 220, 0.12, rough_range=(0.55, 0.75), detail=4)
    elif kind == "walnut":
        m = material("walnut", (0.1, 0.05, 0.025), roughness=0.45, coat=0.4)
        nt = m.node_tree
        b = nt.nodes["Principled BSDF"]
        coord = nt.nodes.new("ShaderNodeTexCoord")
        mapping = nt.nodes.new("ShaderNodeMapping")
        mapping.inputs["Scale"].default_value = (2.5, 0.25, 1)
        wave = nt.nodes.new("ShaderNodeTexWave")
        wave.inputs["Scale"].default_value = 1.2
        wave.inputs["Distortion"].default_value = 9
        wave.inputs["Detail"].default_value = 6
        ramp = nt.nodes.new("ShaderNodeValToRGB")
        ramp.color_ramp.elements[0].color = (0.035, 0.016, 0.008, 1)
        ramp.color_ramp.elements[1].color = (0.32, 0.16, 0.07, 1)
        nt.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
        nt.links.new(mapping.outputs["Vector"], wave.inputs["Vector"])
        nt.links.new(wave.outputs["Fac"], ramp.inputs["Fac"])
        nt.links.new(ramp.outputs["Color"], b.inputs["Base Color"])
    elif kind == "smoked_acrylic":
        m = material("black glass", (0.006, 0.006, 0.008), roughness=0.03, coat=1.0)
    else:
        raise ValueError(kind)
    return m


def switch_materials(kind):
    if kind == "chrome":
        return rp.material("chrome", (0.85, 0.86, 0.88), metallic=1.0, roughness=0.12)
    if kind == "satin":
        return rp.material("satin", (0.75, 0.76, 0.78), metallic=1.0, roughness=0.35)
    if kind == "brass":
        return rp.material("brass", (0.9, 0.68, 0.36), metallic=1.0, roughness=0.22)
    return rp.material("black switch", (0.02, 0.02, 0.022), metallic=0.5, roughness=0.4)


# ---- stages -------------------------------------------------------------------------------------------------------


def world(color, strength=1.0):
    w = bpy.data.worlds.new("world")
    w.use_nodes = True
    bg = w.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (*color, 1)
    bg.inputs["Strength"].default_value = strength
    bpy.context.scene.world = w


def floor(mat):
    bpy.ops.mesh.primitive_plane_add(size=80, location=(0, 0, 0))
    bpy.context.object.data.materials.append(mat)


def stage(kind, type_id):
    if kind in ("studio_white", "studio_pastel", "studio_grey"):
        tone = {"studio_white": (0.78, 0.77, 0.75), "studio_grey": (0.10, 0.10, 0.105),
                "studio_pastel": tuple(0.55 + 0.4 * c for c in hex_rgb(pedal_colour(type_id, 0)))}[kind]
        f = material("seamless", tone, roughness=0.9)
        floor(f)
        area("key", (1.0, 0.98, 0.95), 5200, (-10, 14, 34), (0, 0, 0), 26)
        area("fill", (0.92, 0.95, 1.0), 1500, (16, -10, 26), (0, 0, 0), 30)
        softbox("softbox top", (1, 1, 1), 1.6, (0, 0, 30), (44, 52), gradient=True)
        world((0.5, 0.5, 0.52), 0.35 if kind != "studio_grey" else 0.12)
    elif kind in ("warm_wood", "warm_dark"):
        f = material("wood floor", (0.2, 0.11, 0.06), roughness=0.55)
        nt = f.node_tree
        b = nt.nodes["Principled BSDF"]
        mapping = nt.nodes.new("ShaderNodeMapping")
        mapping.inputs["Scale"].default_value = (0.12, 0.01, 1)
        coord = nt.nodes.new("ShaderNodeTexCoord")
        wave = nt.nodes.new("ShaderNodeTexWave")
        wave.inputs["Distortion"].default_value = 6
        wave.inputs["Detail"].default_value = 4
        ramp = nt.nodes.new("ShaderNodeValToRGB")
        dark = kind == "warm_dark"
        ramp.color_ramp.elements[0].color = (0.02, 0.01, 0.005, 1) if dark else (0.09, 0.04, 0.018, 1)
        ramp.color_ramp.elements[1].color = (0.06, 0.03, 0.015, 1) if dark else (0.3, 0.15, 0.07, 1)
        nt.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
        nt.links.new(mapping.outputs["Vector"], wave.inputs["Vector"])
        nt.links.new(wave.outputs["Fac"], ramp.inputs["Fac"])
        nt.links.new(ramp.outputs["Color"], b.inputs["Base Color"])
        floor(f)
        area("key", (1.0, 0.82, 0.62), 4200 if not dark else 3000, (-12, 16, 30), (0, 0, 0), 18)
        area("fill", (0.7, 0.8, 1.0), 600, (18, -8, 20), (0, 0, 0), 20)
        softbox("softbox top", (1.0, 0.85, 0.7), 2.0, (0, 0, 30), (44, 52), gradient=True)
        world((0.02, 0.015, 0.01), 1.0)
    else:  # stage: the neon look's dark floor and coloured side lights
        f = material("floor", (0.07, 0.07, 0.075), roughness=0.85)
        add_noise_bump(f, 2.5, 0.6, rough_range=(0.55, 0.95))
        rp.floor_albedo(f)
        floor(f)
        left, right = "19c6d6", "e0389b"
        area("key", (1.0, 0.92, 0.85), 2200, (0, 14, 32), (0, -1, 0), 12)
        rp.spot("left", hex_rgb(left), 9000, (-30, 6, 9), (0, 0, 1), size_deg=70)
        rp.spot("right", hex_rgb(right), 9000, (30, -6, 9), (0, 0, 1), size_deg=70)
        softbox("softbox top", (1.0, 0.95, 0.9), 0.5, (0, 0, 30), (44, 52), gradient=True)
        softbox("softbox left", hex_rgb(left), 3.0, (-14, 0, 12), (6, 40))
        softbox("softbox right", hex_rgb(right), 3.0, (14, 0, 12), (6, 40))
        world((0.004, 0.004, 0.007))


# ---- the pedal ----------------------------------------------------------------------------------------------------

GLYPH_SIZE, GLYPH_Y = 2.2, 0.35  # cm per glyph unit (36 frame px), glyph centre


def glyph_polylines(g):
    """The 124x200 frame (centre 62, 98; y down) -> cm on the face"""
    tf = lambda x, y: ((x - 62) / 36 * GLYPH_SIZE, GLYPH_Y - (y - 98) / 36 * GLYPH_SIZE)
    strokes = [[tf(x, y) for x, y in s] for s in g["strokes"] if len(s) > 1]
    dots = [(*tf(x, y), r / 36 * GLYPH_SIZE) for x, y, r in g["dots"]]
    return strokes, dots


def build(style, type_id, glyph):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    st = STYLES[style]
    corner = st["corner"]

    body = body_material(st["body"], type_id)
    hole = material("hole", (0, 0, 0), roughness=0.9)
    rubber = material("rubber", (0.012, 0.012, 0.012), roughness=0.6)
    chrome = material("chrome", (0.85, 0.86, 0.88), metallic=1.0, roughness=0.12)
    sw_mat = switch_materials(st["switch"])

    bevel = 0.22 if st["plate"] is None else 0.18
    outline = rp.rounded_rect(0, 0, W - 2 * bevel, L - 2 * bevel, max(corner - bevel, 0.05), 14)
    curve_object("enclosure", [outline], True, (H - bevel) / 2, bevel=bevel, extrude=(H - bevel) / 2, fill=True,
                 mat=body, resolution=6)
    top = H
    if st["plate"]:
        rim = st.get("rim", 0.22)
        pm = plate_material(st["plate"])
        plate = rp.rounded_rect(0, 0, W - 2 * rim, L - 2 * rim, max(corner - rim * 0.8, 0.05), 14)
        thick = 0.12 if st["plate"] == "smoked_acrylic" else 0.01
        curve_object("face plate", [plate], True, H + thick, extrude=thick, fill=True, mat=pm)
        top = H + 2 * thick

    # the glyph
    strokes, dots = glyph_polylines(glyph)
    kind = st["glyph"]
    if kind == "neon":
        c = hex_rgb(pedal_colour(type_id, 2))
        ink, z, r = material("neon", c, emission=c, strength=2.2), top + 0.035, 0.055
    elif kind == "edgelit":  # lit lines inside the acrylic, under its surface
        c = hex_rgb(pedal_colour(type_id, 2))
        ink, z, r = material("edge lit", c, emission=c, strength=3.0), top - 0.01, 0.03
    elif kind == "print":  # screen-printed ink: a thin flat line barely raised
        ink, z, r = material("ink", hex_rgb(st["ink"]), roughness=0.5), top - 0.005, 0.042
    elif kind == "engrave":  # cut into the metal: only the top of a dark line shows
        ink, z, r = material("engraving", hex_rgb(st["ink"]), metallic=0.8, roughness=0.55), top - 0.003, 0.045
    else:  # inlay: metal wire set flush into the wood
        ink, z, r = material("inlay", hex_rgb(st["ink"]), metallic=1.0, roughness=0.2), top - 0.002, 0.055
    g = curve_object("glyph", strokes, False, z, bevel=r, mat=ink)
    g.data.use_fill_caps = False
    for i, (x, y, rr) in enumerate(dots):
        cylinder(f"dot {i}", max(rr, r), 0.08, (x, y, z), ink, verts=32)
    if kind in ("print", "engrave", "inlay"):  # a flat-topped look: squash the tubes
        g.scale.z = 0.25

    # LED and footswitch
    led_y, sw_y = -3.15, -4.75
    if st["led"] == "bezel":
        torus("led bezel", 0.24, 0.05, (0, led_y, top + 0.02), sw_mat)
        cylinder("led hole", 0.22, 0.04, (0, led_y, top), hole, verts=48)
    else:  # a small flush hole
        cylinder("led hole", 0.16, 0.04, (0, led_y, top - 0.015), hole, verts=48)
    clean = st["switch"] in ("satin", "black")
    if clean:  # a low, simple round switch
        cylinder("switch ring", 0.82, 0.06, (0, sw_y, top + 0.03), sw_mat, bevel=0.02)
        cylinder("switch cap", 0.68, 0.5, (0, sw_y, top + 0.3), sw_mat, bevel=0.12)
    else:
        cylinder("switch nut", 0.78, 0.16, (0, sw_y, top + 0.08), sw_mat, verts=6, bevel=0.03, smooth=False)
        cylinder("switch washer", 0.85, 0.04, (0, sw_y, top + 0.02), sw_mat, bevel=0.01)
        cylinder("switch bushing", 0.58, 0.3, (0, sw_y, top + 0.3), sw_mat)
        cylinder("switch cap", 0.5, 0.75, (0, sw_y, top + 0.75), sw_mat, bevel=0.1)

    # jacks and cables
    jack_y, jack_z = 0.9, H * 0.55
    cable_mat = rubber
    for side in (-1, 1):
        x = side * W / 2
        rot = (0, math.pi / 2, 0)
        cylinder("jack nut", 0.5, 0.18, (x + side * 0.09, jack_y, jack_z), chrome, rot=rot, verts=6, bevel=0.02,
                 smooth=False)
        cylinder("plug", 0.34, 1.1, (x + side * 0.72, jack_y, jack_z), chrome, rot=rot, bevel=0.05)
        cylinder("boot", 0.36, 1.2, (x + side * 1.85, jack_y, jack_z), cable_mat, rot=rot, bevel=0.08)
        cylinder("cable", 0.24, 4.0, (x + side * 4.3, jack_y, jack_z * 0.8), cable_mat,
                 rot=(0, math.pi / 2 - side * 0.1, 0))

    stage(st["stage"], type_id)

    cam = bpy.data.cameras.new("camera")
    cam.lens = 100
    cam.sensor_fit = "VERTICAL"
    cam.sensor_height = 24
    cam_ob = bpy.data.objects.new("camera", cam)
    cam_ob.location = (0, 0, rp.FRAME_H / (cam.sensor_height / cam.lens))
    bpy.context.collection.objects.link(cam_ob)
    scene.camera = cam_ob

    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = rp.RES
    glowing = kind in ("neon", "edgelit")
    scene.view_settings.view_transform = "Khronos PBR Neutral" if glowing else "AgX"
    if not glowing:
        scene.view_settings.look = "AgX - Medium High Contrast"
    scene.render.image_settings.file_format = "PNG"
    return scene, glowing


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("style", choices=sorted(STYLES))
    parser.add_argument("type_ids", nargs="+")
    parser.add_argument("--glyphs", required=True)
    parser.add_argument("--out", default=".")
    parser.add_argument("--samples", type=int, default=96)
    parser.add_argument("--scale", type=int, default=100, help="resolution percentage")
    args = parser.parse_args(argv)
    with open(args.glyphs) as f:
        glyphs = json.load(f)
    os.makedirs(args.out, exist_ok=True)
    for type_id in args.type_ids:
        scene, glowing = build(args.style, type_id, glyphs[type_id])
        scene.cycles.samples = args.samples
        scene.render.resolution_percentage = args.scale
        png = os.path.abspath(os.path.join(args.out, f"{args.style}-{type_id}.png"))
        scene.render.filepath = png
        bpy.ops.render.render(write_still=True)
        if glowing:
            rp.bloom(png, ("19c6d6", "e0389b"))
        print("wrote", png)


if __name__ == "__main__":
    main()
