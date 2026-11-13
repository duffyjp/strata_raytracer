scene.pak - asset archive for raytracer.exe
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
      tools\glslang\bin\glslang.exe -V --target-env vulkan1.3 -e main ^
        -o shaders\main.spv build\generated\glsl\main.rgen

audio/*.ogg                          (reserved)
    The archive format already carries them; the app has no audio output yet.

Repack after editing:
    python tools\make_pak.py
