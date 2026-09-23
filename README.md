# Open1560

![Preview](extra/preview.png)

[![Download Latest Version](https://img.shields.io/badge/download-latest-brightgreen?logo=data%3Aimage%2Fsvg%2Bxml%3Bbase64%2CPHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIGhlaWdodD0iMjQiIHdpZHRoPSIyNCI%2BPHBhdGggZmlsbD0iIzRjMSIgZD0iTTUgMjBoMTR2LTJINXYyek0xOSA5aC00VjNIOXY2SDVsNyA3IDctN3oiLz48L3N2Zz4%3D)](https://0x1f9f1.github.io/Open1560)
[![Chat On Discord](https://img.shields.io/discord/239900961731117059?color=7289DA&logo=discord)](https://discord.gg/HHZz27sFEH)

Open1560 is an open source re-implementation of Midtown Madness Sneak Preview Beta / Build 1560.

This project is a partial rewrite of Midtown Madness 1, using assembly to provide functions that have not yet been reimplemented in C++.<br/>
The intention is to allow the fixing of bugs, implementation of new features and porting to platforms unsupported by the original.<br/>

## Changes

Notable changes include:
* OpenGL Renderer
* DirectX 9 Renderer (see below)
* SDL Gamepad Support
* Audio Fixes
* Crash Fixes
* Input Fixes
* Stuttering Fixes
* Improved Debug Menu
* Improved Performance
* Improved/Fixed Text Rendering

## DirectX 9 renderer

`code/midtown/agidx9` is a second hardware backend alongside the OpenGL one. It exists mainly so the
game can be path traced by [NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix), which
reconstructs a scene from the fixed-function draw state a D3D9 game submits.

That goal decides how the backend is built. Remix can only reconstruct geometry it receives in
**model space with real `SetTransform(WORLD/VIEW/PROJECTION)` calls**. A draw that arrives already
transformed on the CPU (`D3DFVF_XYZRHW`) carries no world-space information and is skipped, and a
draw issued through a vertex shader is opaque to it, since Remix cannot know what the shader did to
the position. So this backend moves as much of the frame as possible onto a world-space path
(`agiDX9Rasterizer::MeshWorld`), and keeps the fixed-function pipeline rather than a programmable
one. In a city, world-space submission currently accounts for roughly **98-99%** of scene triangles;
the remainder is HUD, text and the minimap, which are meant to be 2D. `-ghash` makes the renderer
report that split, and its Remix geometry-hash stability, every 120 frames.

A programmable path (`dx9shader.cpp`, `dx9probe.cpp`, `game/hlsl`) was written and then unwired for
the same reason. It still compiles, nothing calls it, and the switches that fed it are listed as
inert below.

### Selecting it

Pass `-d3d9`. Fullscreen, resolution and colour depth otherwise come from the game's own graphics
settings; `-width`/`-height` override the resolution for one run.

### How it presents

The engine draws in a coordinate space of the pipeline's resolution ("logical pixels"), but the D3D9
device is created at the **window** size, which in fullscreen is the whole display. Those two
disagree whenever the game is not running at the desktop resolution, and in the menus they always
disagree, because the menu pipeline is deliberately built at a fixed 640x480 - the size the menu
system's widget art and bitmap fonts were authored for, and the only size at which the whole menu
scales as one image.

Rather than render to a texture and blit it (what the OpenGL backend does, and what would hide
gameplay from Remix), the gap is closed as a coordinate transform: screen-space vertices and viewport
rectangles are mapped from logical pixels onto the blit rectangle. Remix ignores both kinds of thing,
so this costs it nothing. Rasterisation still happens at full device resolution - only the layout is
640x480. `-scaling` picks the rectangle; the default preserves the 4:3 aspect and pillarboxes.

### Command line switches

Every switch below is also visible at runtime with `-help`, which lists all registered parameters
with their descriptions. Boolean switches take `0` or `1`, and default to off unless stated.

**Selection and display**

| Switch | Default | Effect |
| --- | --- | --- |
| `-d3d9` | off | Select the DirectX 9 renderer. |
| `-d3d9dll <name>` | `d3d9_remix.dll`, then `d3d9.dll` | Which DLL to take `Direct3DCreate9` from. |
| `-width <n>` / `-height <n>` | selected mode | Pipeline resolution. |
| `-depth <n>` | 32 | Colour depth. |
| `-vsync <0/1>` | 1 | Wait for vertical blank. |
| `-scaling <0-3>` | 0 | 0 stretch keeping aspect, 1 stretch, 2 centred, 3 centred integer-scaled. |
| `-border <0/1>` | 1 | Window border, windowed only. |
| `-menunative` | off | Compose the menus at the selected resolution instead of 640x480. Backgrounds then fill the screen while widgets, sliders and fonts stay their authored pixel size - see "How it presents". |

**Renderer**

| Switch | Default | Effect |
| --- | --- | --- |
| `-aniso <n>` | 16 | Anisotropic filtering, clamped to the device maximum. `1` disables it. Applied to minification only. |
| `-d3d9frameclear <0/1>` | 1 | Clear colour and depth at the start of each frame. |
| `-d3d9scenetarget` | off | Render the scene into an offscreen target and blit it back. Exercises the render-target framework; **invisible to Remix**. |
| `-d3d9specular` | off | Add a specular term to the static city lighting rig. The original rig has no specular concept at all. |
| `-d3d9nofx` | off | Skip the chrome and ground-map second passes. They duplicate geometry, which matters for a Remix capture, and cost real visuals otherwise. |
| `-d3d9depthbias <f>` | 0.0 | Depth bias for hardware-transformed geometry. Should no longer be needed. |
| `-d3d9worldfog <0/1>` | off under Remix, on otherwise | Fixed-function fog on world draws. Remix rebuilds this state as its own volumetric fog, which washes the city out white, so it is off when the Remix bridge is detected. Pass `0` for a Remix install that is not detected, or `1` to feed Remix the fog anyway. |
| `-noskin` | off | Disable hardware matrix-palette skinning; submit one draw per bone instead. |
| `-ffperpixel` | off | Per-pixel Blinn-Phong for the sun, through the texture-blending unit. Costs extra passes and is **not Remix compatible**. |
| `-ffperpixelsteps <0-6>` | 4 | Blinn exponent for the above, as squaring steps (2^n). |
| `-ffperpixelreflect` | off | Generate vehicle reflection coordinates per pixel instead of per vertex. |

**World geometry**

These act on the world-space path, so they apply to this backend only.

| Switch | Default | Effect |
| --- | --- | --- |
| `-nocull` | off | Disable backface, LOD and distance culling. A path tracer wants closed shells; a back face it never receives is a hole light leaks through. |
| `-smoothnormals <0/1>` | 1 | Rebuild smooth vertex normals in float. The engine stores normals as an index into a 198-entry table, coarse enough that a facet's corners often quantise to one direction and shade flat. |
| `-flatnormals` | off | Shade from facet geometry, ignoring stored vertex normals. |
| `-nativecpucull` | off | Cull backfacing facets on the CPU. **Breaks Remix hash stability.** |
| `-pedskin` | off | Skin pedestrians on the CPU. **Breaks Remix hash stability.** |
| `-pedsticks` | off | Draw distant pedestrians as the original's stick figures (one screen-space ribbon per bone) instead of the full skinned mesh. **Invisible to Remix** - by default every pedestrian draws as a world-space mesh at every distance. |
| `-reflectamount <f>` | 0.35 | Vehicle sphere-map reflection strength. |
| `-reflectfresnelbias <f>` | 1.0 | Vehicle reflection fresnel bias. |
| `-reflectfresnelscale <f>` | 0.0 | Vehicle reflection fresnel scale. |
| `-reflectspecular <f>` | 0.0 | Vehicle reflection sun-glint strength. |
| `-trafficrefl` | off | Sphere-map reflections on AI and traffic vehicles too. |
| `-worldlinewidth <f>` | 0.02 | World-space half-width of spark and debug lines. |
| `-worldlinegrow <f>` | 0.0015 | Extra line half-width per unit of view depth. |

**Diagnostics and escape hatches**

| Switch | Default | Effect |
| --- | --- | --- |
| `-ghash` | off | Report Remix geometry-hash stability for world draws: distinct hashes, how many are new this frame, and which textures the churn belongs to. |
| `-ghashcolor` | off | Tint world draws by geometry hash, as Remix's own Geometry Hash debug view does. A stable mesh holds one colour; CPU-pretransformed geometry is marked flat magenta. |
| `-d3d9legacydepth` | off | Fold `agiMeshSet::DepthScale`/`DepthOffset` back into the projection matrix. **Breaks Remix** - it produces a frustum with no far plane, which is why it is not the default. |
| `-d3d9rhview` | off | Hand Remix a right-handed view matrix, folding the Z flip into the projection instead. Clip space is identical either way, so this is an A/B switch rather than a rendering change. |
| `-d3d9identityproj` | off | Reset `PROJECTION` to identity after world draws. **Breaks Remix.** |
| `-d3d9nostatecache` | off | Send every render state, transform, texture binding, light and material to the device even when unchanged. Diagnostic: if the picture changes with it on, something is writing the device behind the state filter. |
| `-d3d9attribution` | off | Name screen draws by texture in the periodic census log (dropped, submitted, in-scene). Costs a string search per screen draw, so it is off unless you are chasing a missing or CPU-pretransformed surface. |

**RTX Remix API**

The game's street lamps, traffic signals and vehicle lights are glow sprites that light nothing in
the original. With `-remixapi`, each one is sent to Remix as a real light through the Remix API.
This works with NVIDIA's RTX Remix bridge and with
[Remix Plus](https://github.com/RemixProjGroup/dxvk-remix), and needs `exposeRemixApi = True` in
`bridge.conf` (in the `.trex` folder next to the game's `d3d9.dll`); without that line the bridge
refuses and the log says so. The log also names which bridge it found. The design, what each bridge
does and does not forward to a 32-bit game, and the phases still to come are in
[docs/remix_api_plan.md](docs/remix_api_plan.md).

| Switch | Default | Effect |
| --- | --- | --- |
| `-remixapi` | off | Connect to the Remix API and send glow lights. |
| `-remixlightpower <f>` | 1.5 | Overall brightness of those lights. The `-light*` multipliers below scale on top of it. |
| `-remixlightradius <f>` | 0.15 | Size of each light's emitter, in world units. Brightness does not depend on it; it sets how soft the shadows are. |
| `-remixmaxlights <n>` | 192 | Most lights sent per frame; the brightest are kept. |
| `-remixheadlights` | off | Also send headlight cones. Off because the cone's centre sits metres ahead of the car, so it lights the road from the wrong place. |
| `-remixconfig <k=v\|...>` | none | Remix options (`rtx.conf` keys) applied once the API connects, separated by `\|`, e.g. `rtx.fallbackLightMode=0`. |
| `-remixapidebug` | off | Log the first 64 lights as they are created. |
| `-glowheadlights`, `-glowvehiclelights`, `-glowtrafficlights`, `-glowstreetlamps`, `-glowgenericlights` | on | Which kinds of glow emit light at all. |
| `-lighthead`, `-lightvehicle`, `-lighttraffic`, `-lightlamp`, `-lightgeneric` | 0.05, 1.25, 2.0, 10.0, 1.0 | Per-kind brightness. |
| `-glowreachscale <f>`, `-glowreachmin <f>` | 14, 20 | Convert a flare's drawn size into how far it throws. Brightness goes with the square of this. |
| `-glowdebug` | off | Log each glow texture as it is first harvested. |

A census line, `DX9 REMIXAPI`, reports live lights and how many were created, re-sent and destroyed
every 120 frames.

**Inert - the unwired programmable path**

These are still registered so an existing `Open1560-Shaders.ini` does not start warning about unknown
keys, but nothing reads them at runtime: `-d3d9quality`, `-d3d9sun`, `-d3d9reflect`, `-d3d9tonemap`,
`-d3d9exposure`, `-d3d9heightfog`, `-d3d9flashpower`, `-d3d9glowlights`, `-d3d9glowpower`,
`-d3d9cellsize`, `-d3d9lightspec`, `-d3d9cellpack`.

### Open1560-Shaders.ini

The renderer writes a fully commented `Open1560-Shaders.ini` next to the executable on first run.
Every key in it is one of the switches above, applied through the same mechanism - so anything
tunable on the command line is tunable from the file and vice versa, and the command line wins, which
lets a setting be overridden for one run without editing the file. Delete it to regenerate it. It
predates the tables above and covers mostly the inert keys; the tables are the complete set.
