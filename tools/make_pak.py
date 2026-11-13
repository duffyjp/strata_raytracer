#!/usr/bin/env python3
"""make_pak.py - generate the asset files and pack them into scene.pak (a plain ZIP).

The archive is deliberately a normal .zip so any tool can open it: the application reads the
central directory itself and accepts stored or deflated entries. Adding a file to assets/ and
re-running this script is all it takes to make new art appear in the render.
"""
import argparse
import json
import os
import struct
import sys
import zlib
import zipfile

# ------------------------------------------------------------------ procedural PNG assets
def png_bytes(width, height, pixel_fn):
    """Encodes an RGBA PNG without any third-party dependency."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type: none
        for x in range(width):
            r, g, b, a = pixel_fn(x, y)
            raw += bytes((r & 255, g & 255, b & 255, a & 255))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def make_checker(path):
    """Ground albedo: a soft two-tone checker with a little grain so it reads as a studio floor."""
    def px(x, y):
        cell = ((x // 16) + (y // 16)) & 1
        edge = (x % 16 == 0) or (y % 16 == 0)
        base = (168, 170, 178) if cell == 0 else (58, 62, 72)
        if edge:
            base = tuple(int(c * 0.72) for c in base)
        n = ((x * 7 + y * 13) % 9) - 4
        return (base[0] + n, base[1] + n, base[2] + n, 255)
    data = png_bytes(128, 128, px)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


def make_ramp(path):
    """Warm-to-cool ramp used as an emissive tint if a material points at it."""
    def px(x, y):
        t = x / 63.0
        r = int(255 * (1.0 - t) ** 1.2 + 40)
        b = int(255 * t ** 1.2 + 40)
        g = int(150 + 60 * (1.0 - abs(t - 0.5) * 2.0))
        return (min(r, 255), g, min(b, 255), 255)
    data = png_bytes(64, 8, px)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


SCENE_JSON = {
    "comment": "Scene description. Edit, repack with tools/make_pak.py, restart the app.",
    "exposure": 1.0,
    "maxBounces": 6,
    "accumulationFrames": 1,
    "autoQuality": True,
    "targetFps": 60.0,
    "qualityHeadroom": 0.26,
    "maxSamplesPerFrame": 16,
    "minRenderScale": 0.40,
    "baseSamplesPerFrame": 4,
    "maxRenderScale": 2.0,
    "maxTracePixels": 8500000,
    "radianceClamp": 25.0,
    "skyColor": [0.30, 0.34, 0.42],
    "camera": {
        "orbitRadius": 11.5,
        "orbitHeight": 3.4,
        "heightBob": 0.7,
        "speed": 0.24,
        "fovDeg": 42.0,
        "lookAt": [0.0, 0.85, 0.0],
    },
    "materials": [
        {"name": "ground", "model": 0, "albedo": [0.52, 0.53, 0.56], "reflectivity": 0.12,
         "roughness": 0.35, "ior": 1.4, "texture": "checker", "textureScale": 1.0},
        {"name": "mirror", "model": 1, "albedo": [0.95, 0.96, 0.97], "reflectivity": 0.95,
         "roughness": 0.02},
        {"name": "satin", "model": 1, "albedo": [0.86, 0.87, 0.90], "reflectivity": 0.70,
         "roughness": 0.16},
        {"name": "red_dull", "model": 0, "albedo": [0.82, 0.20, 0.16], "reflectivity": 0.05,
         "roughness": 0.60},
        {"name": "glass", "model": 2, "albedo": [1.0, 1.0, 1.0], "reflectivity": 0.05,
         "roughness": 0.01, "ior": 1.52},
        {"name": "mirror_cube", "model": 1, "albedo": [0.90, 0.92, 0.95], "reflectivity": 0.85,
         "roughness": 0.05},
        {"name": "teal", "model": 0, "albedo": [0.18, 0.55, 0.47], "reflectivity": 0.06,
         "roughness": 0.55},
        {"name": "light_warm", "model": 3, "albedo": [0.1, 0.08, 0.06],
         "emissive": [30.0, 20.0, 11.0]},
        {"name": "light_cool", "model": 3, "albedo": [0.06, 0.08, 0.10],
         "emissive": [12.0, 19.0, 30.0]},
    ],
    "lights": [
        {"name": "key", "color": [1.0, 0.78, 0.55], "intensity": 48.0, "radius": 0.34,
         "orbitRadius": 3.9, "orbitHeight": 2.6, "speed": 0.55, "phase": 0.0},
        {"name": "fill", "color": [0.45, 0.66, 1.0], "intensity": 30.0, "radius": 0.27,
         "orbitRadius": 4.7, "orbitHeight": 1.7, "speed": -0.40, "phase": 2.1},
    ],
    "textures": [
        {"name": "checker", "file": "textures/checker.png", "scale": 6.0},
        {"name": "ramp", "file": "textures/emissive_ramp.png", "scale": 1.0},
    ],
}

README = """scene.pak - asset archive for raytracer.exe
=========================================

This is a plain ZIP file. Open it with Explorer/7-Zip, edit a file, save it back and restart the
application. Compression may be 'stored' or 'deflated'; both are supported, CRCs are verified.
Do not use ZIP64 (archives beyond ~4 GB / 65535 entries are rejected).

Contents and what each file does
--------------------------------
scene.json
    Camera orbit (orbitRadius, orbitHeight, heightBob, speed, fovDeg, lookAt), renderer settings
    (exposure, maxBounces, accumulationFrames, radianceClamp, skyColor), the material table
    (albedo, emissive, reflectivity, roughness, ior, texture, textureScale, model), the two light
    definitions and the texture list. Materials are matched by name against the scene objects, so
    editing a value here changes the render with no rebuild. A missing or broken file falls back to
    the compiled-in defaults and says so on stdout.

textures/checker.png, textures/emissive_ramp.png
    Decoded at start-up and bound as slots 0 and 1. The 'ground' material references 'checker'; if
    a texture is missing the shader substitutes a procedural checker so the picture stays complete.
    Add your own: drop the file in, list it under "textures" in scene.json, then point a material at
    it with "texture": "<name>". Slots beyond the first few are ignored (max 8).

shaders/*.spv                        (optional, not shipped by default)
    Any file named here replaces the compiled-in SPIR-V for that stage, letting you iterate on the
    lighting without rebuilding the exe:
      shaders/main.spv          ray generation (camera + transport loop)
      shaders/hit.spv           closest hit (geometry report)
      shaders/miss.spv          miss / environment
      shaders/shadow_hit.spv    closest hit for shadow rays
      shaders/shadow_miss.spv   miss for shadow rays
      shaders/fallback.spv      ray-query compute path
      shaders/present.frag.spv  tone map / present
    Build one with:
      tools\\glslang\\bin\\glslang.exe -V --target-env vulkan1.3 -e main ^
        -o shaders\\main.spv build\\generated\\glsl\\main.rgen

audio/*.ogg                          (reserved)
    The archive format already carries them; the app has no audio output yet.

Repack after editing:
    python tools\\make_pak.py
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--out", default=None)
    ap.add_argument("--deflate", action="store_true", help="deflate instead of storing")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    assets = os.path.join(root, "assets")
    tex_dir = os.path.join(assets, "textures")
    os.makedirs(tex_dir, exist_ok=True)

    checker = os.path.join(tex_dir, "checker.png")
    ramp = os.path.join(tex_dir, "emissive_ramp.png")
    print("  checker.png      %d bytes" % make_checker(checker))
    print("  emissive_ramp.png %d bytes" % make_ramp(ramp))

    scene_path = os.path.join(assets, "scene.json")
    if os.path.isfile(scene_path):
        # Keep an author-edited file valid rather than clobbering it.
        try:
            with open(scene_path, "r", encoding="utf-8") as f:
                json.load(f)
            print("  scene.json       kept (author-edited)")
        except Exception as e:  # noqa: BLE001
            print("  scene.json       invalid (%s); regenerating" % e)
            with open(scene_path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(SCENE_JSON, f, indent=2)
                f.write("\n")
    else:
        with open(scene_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(SCENE_JSON, f, indent=2)
            f.write("\n")
        print("  scene.json       written")

    with open(os.path.join(assets, "README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(README)

    out = args.out or os.path.join(root, "build", "bin", "scene.pak")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    compression = zipfile.ZIP_DEFLATED if args.deflate else zipfile.ZIP_STORED
    names = ["scene.json", "README.txt", "textures/checker.png", "textures/emissive_ramp.png"]
    with zipfile.ZipFile(out, "w", compression=compression) as z:
        for n in names:
            src = os.path.join(assets, n.replace("/", os.sep))
            if not os.path.isfile(src):
                print("  missing %s - skipped" % n)
                continue
            z.write(src, n)
            print("  packed %-28s %7d bytes" % (n, os.path.getsize(src)))

    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
