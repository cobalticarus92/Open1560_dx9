# Open1560 — engine data sources for the RTX Remix API

Pathway B (the `ps_3_0` programmable renderer) has been removed. Before it went, it had worked out
where in this engine to find things the original game never exposes as data: where its light sources
actually are, what colour they emit, what the sun is doing, what a surface is made of. None of that
was Pathway B's own invention — it is all recovered from the draw stream and from engine state — and
none of it is specific to having a pixel shader.

**This document exists so that knowledge survives the deletion.** The intended consumer is the RTX
Remix API, which wants exactly the same facts (here is a light, here is its position and colour and
radius; this material is emissive; the sun is here) and cannot get them from a D3D9 draw call either.

The code is not gone, only unreferenced: everything described here can be read at commit
`0dac83657` and its ancestors, principally `code/midtown/agiworld/glowlight.h`,
`code/midtown/agidx9/dx9shader.cpp` and `code/midtown/agidx9/dx9probe.cpp`.

How this data now reaches the runtime - the bridge's constraints, what is built and what comes next -
is in [remix_api_plan.md](remix_api_plan.md). The glow harvest described in §1 is live again there,
behind `-remixapi`.

A caveat on the Remix side: this document is written from the engine outward. It says what data
exists, where to get it, and what will go wrong when you do. It does not attempt to specify exact
`remixapi_*` struct layouts or call sequences — check those against the Remix headers you are
building against rather than against anything asserted here.

---

## 1. The headline: the draw stream is a light database

MM1's city is full of things that look like light sources — street lamps, traffic signals, lit
signage, headlights, tail and brake lights, coronas. **The original engine emits no light from any of
them.** They are `agiTexProp::AlphaGlow` billboards composited additively, and nothing in the engine
believes they illuminate anything.

But the draw carries everything a real light needs: a world-space position, a colour, and a size. So
the draw stream *is* the light database — no new assets, no per-city hand placement, and it works for
every city including ones nobody has seen.

This is the single highest-value thing Pathway B established, and it transfers to Remix light
injection essentially unchanged.

### 1.1 There are two harvest routes and you need both

Glows reach the renderer by two completely separate paths. Covering only the obvious one misses every
vehicle light in the game.

| Route | Entry point | What comes through it |
|---|---|---|
| Billboards | `agiMeshSet::DrawCard` | Bangers: street lamps, traffic signals, lit signage |
| Glow **meshes** | `agiRasterizer::MeshWorld` | All vehicle lighting — head, tail, brake, reverse |

Vehicle lighting never touches `DrawCard`. `mmCarModel::DrawGlow` (player) and
`aiVehicleInstance::DrawGlow` (traffic) both select a glow mesh out of `mmInstance::MeshSetTable` and
submit it through `agiMeshSet::Draw`.

### 1.2 `DrawCard`'s position is MODEL space — the trap that cost the most

This one hid for a long time because it is invisible for half the callers.

`DrawCard` projects its position through `view_params.ModelView` (= View × World), so the **current
world matrix applies**. `asParticles::Cull()` calls `SetWorld(IDENTITY)` before its cards, so for
particles, smoke and vehicle glows model space *is* world space and reading the raw position is
accidentally correct. `mmBangerInstance::DrawGlow()` does **not** — it sets the banger's own
transform first.

Every street lamp therefore registered its light at the raw `mmBangerData::GlowOffset`: measured as
`(0.0, 1.8, 0.0)` for `opstlite` and `(-2.3, 6.3, 0.0)` for `opstlite_blue`. Not positions in Chicago
— the offsets themselves. Two compounding consequences: every lamp in the city piled up within a
couple of metres of the world origin, **and** because they landed on identical coordinates the slot
matcher merged them all into a *single* light. The entire city's street lighting was one light at the
origin. Nothing looked broken, because the flare still drew in the right place — only the harvest
took the number at face value.

**Fix: transform by `ViewParams().World`**, which is the matrix `DrawCard` is already implicitly
using, and which collapses to a no-op for the identity case.

Verified numerically, which is the only way this kind of thing should be called done: the road's
warmth ratio under a lamp went from 1.05 (indistinguishable from unlit asphalt) to 2.72.

**Generalised rule: when harvesting anything out of a draw call, transform by the matrix that draw is
itself using.** A position is not a position until you know its space.

### 1.3 Colour comes from the texture, sampled at the flare's own UV

The card's vertex tint is *not* the emitted colour. The real hue is that tint modulated by the glow
texture — and one texture routinely holds several differently coloured lamps. A traffic signal sheet
carries red, amber and green side by side, and `DrawCard`'s `frame` argument selects a UV sub-rect
out of `agiMeshCardInfo::Frames`.

So the harvest must record `agiTexDef* Texture` **plus `U`/`V`**, and the renderer resolves the colour
by sampling there. Averaging the whole texture gives every signal the same muddy colour regardless of
state; sampling where the flare actually reads is what makes signals cycle correctly.

Sampling is **luminance-weighted on a coarse grid**, not a flat mean — a glow sheet is mostly black
background by design, and a flat mean drags every light toward black.

For the mesh route the colour arrives already resolved, because there are two sources to combine: the
mesh's per-vertex colour and `agiTexParameters::Color`, the texture's own representative colour
(`agiworld/getmesh.cpp`: `tex.Color = prop->Color`, with a day/night variant in the sheet data). That
is the engine's own answer to "what colour is this light", and it is why a red tail light, an amber
indicator and a green signal all come out right with nothing hard-coded.

### 1.4 Classification: relative saturation, on the resolved colour

Lights need per-kind intensity — a street lamp exists to light a street, a tail light exists to be
seen. The engine records no "kind" field, so it is recovered from the texture name and the emitted
colour:

| Kind | Recovered from |
|---|---|
| `Headlight` | `FXLTCONE` — the headlight cone mesh |
| `Vehicle` | `FXLTGLOWRED` / `FXLTGLOWAMBER` — tail and brake |
| `Traffic` | a pure hue |
| `Lamp` | warm near-white |
| `Generic` | neutral white — reverse lamps, coronas, everything else |

Brake and tail are deliberately **one** kind: both draw `FXLTGLOWRED` off the same mesh with no state
flag between them, so splitting them would be a guess presented as a setting.

Two mistakes here, both of which swapped whole populations:

- **Absolute channel spread does not work.** The first version tested `(peak - floor) > 0.40`. A
  street lamp measures `1.00/0.98/0.47` — spread `0.53` — so *every street lamp in the city*
  classified as a traffic signal. Warm white is by definition a wide absolute spread. It failed the
  other way too: a red tail light at alpha 0.3 arrives as `0.30/0.00/0.00`, spread `0.30`,
  "unsaturated", and classified as a street lamp. Which way a flare went depended on how brightly it
  happened to be drawn that frame. **`(peak - floor)/peak` is scale-invariant** and separates the
  observed populations with room to spare.
- **Unsaturated ≠ street lamp.** Falling back to the lamp multiplier for anything non-pure-hue handed
  the full lamp budget to a neutral white glow on a vehicle. Every lamp in this game is incandescent
  or sodium, so a real one is *warm* — blue well under red. Neutral whites take `Generic`.

Classification must be **re-run by the renderer** against the colour sampled out of the texture, not
just the card's tint, or the two disagree.

### 1.5 Lights must persist across frames

Tying a light's lifetime to whether its sprite was drawn this frame is the wrong lifetime, and it is
what produces flickering: a lamp whose billboard is momentarily culled — off-screen, an LOD switch, a
cell boundary — has its light vanish outright and snap back a frame later. Nothing about the lamp
changed, only whether the renderer drew its sprite.

The working model is slots, not a rebuilt list:

- refreshed when the sprite is seen, updated **in place** so the light tracks its owner within the
  same frame rather than trailing by one (which is what made vehicle lights lag behind moving cars)
- expire on a timer: `TTL = 12` frames, of which the last `FADE = 6` are a linear fade to zero, so a
  light that genuinely switches off (a signal changing state) does not visibly linger, and a
  momentary cull does not blink
- **matching predicts, it does not just compare.** At 200 km/h a car moves about a metre per frame, so
  a fixed distance threshold stops recognising its own headlights, allocates a new slot every frame,
  and leaves a trail of orphans behind the car. Match against the slot's **velocity-predicted**
  position; this makes the threshold speed-independent.

### 1.6 Reach

The engine records nothing about how far a lamp throws. All it has is how big the flare is drawn, so
reach is a multiplier on the flare's half-extent with a floor — both judgement calls, both worth
exposing as settings. Without the floor, a small distant flare produces a light with a metre of reach
that illuminates nothing but itself.

**Both harvest routes must use the same function.** They once disagreed (24 units vs 14), and because
emitted intensity scales with the *square* of reach, the same physical fixture came out ~3× brighter
depending on whether the engine happened to draw it as a card or as geometry.

### 1.7 Teardown is mandatory

`agiGlowLight::Texture` is an `agiTexDef*` borrowed from the draw it was harvested from. The registry
is a process-lifetime global; the texture is owned by the pipeline. **Quitting a race to the menu
destroys the pipeline and resets the engine's memory arena**, freeing every `agiTexDef` wholesale.
Without an explicit reset the registry survives holding dangling pointers and the next frame faults
walking them. The lights describe a city that no longer exists, so there is nothing to preserve.

This same teardown is relevant well beyond lights — see §6.

---

## 2. The sun, time of day and weather

`MMSTATE.TimeOfDay` and `MMSTATE.Weather` are both 0..3 and are the only inputs. There is no latitude,
date or compass in this game, so there is nothing to be astronomically accurate *to*; the useful
target is that each preset be recognisable at a glance and that shadows and highlights fall somewhere
plausible.

These tables are authored data and would otherwise be lost with the code:

| TimeOfDay | Elevation° | Azimuth° | R | G | B | |
|---|---|---|---|---|---|---|
| 0 Morning | 18 | 95 | 1.00 | 0.82 | 0.62 | low, warm, from the east |
| 1 Noon | 74 | 195 | 1.00 | 0.97 | 0.92 | near overhead, almost white |
| 2 Sunset | 9 | 268 | 1.00 | 0.55 | 0.30 | very low, heavily reddened, from the west |
| 3 Night | 52 | 25 | 0.32 | 0.38 | 0.55 | moonlight, cool and dim |

Azimuth is measured `0 = +Z`, increasing toward `+X`. Elevation is above the horizon.

| Weather | Scale | Desaturate | |
|---|---|---|---|
| 0 Sun | 1.00 | 0.00 | keeps the sun's own hue |
| 1 Fog | 0.55 | 0.65 | washed out and neutral |
| 2 Rain | 0.38 | 0.75 | dimmest and coldest |
| 3 Snow | 0.70 | 0.55 | dim sun, but snow scatters a lot back up |

Weather pulls the sun toward neutral and scales its strength: overcast does not merely dim a sunbeam,
it converts it into a diffuse source, so the meaningful knob is *how much key light survives as a
directional term at all*.

**Night is deliberately a moon, not a below-horizon sun.** Sinking it below the horizon would be more
literal and worse: the directional term vanishes entirely, the whole night rig collapses onto ambient,
and it flattens exactly the surfaces — car bodywork — that street lighting is there to shape.

For Remix this maps to a distant light. Note the memoisation lesson from the perf work: derive it once
per frame on the `(time, weather)` pair, never per draw.

---

## 3. Sky, ambient and lightning

- **Sky colour** — `mmSky::Color`, and `SkyColor` in `mmcity/cullcity.cpp`.
- **Static lighting rig** — `agiMeshLighterSun` / `Fill1` / `Fill2` + `agiMeshLighterAmbient` are the
  engine's own three directionals plus ambient for city geometry. Ambient is sourced from the vertex
  colour, and it lands *inside* the product the CPU rig computes: `colour * (key + fill1 + fill2 +
  ambient)`, **not** `colour * (key + fill1 + fill2) + ambient`.
- **Dynamic lights** — `agiLighter::LIGHTS[0, Current)`. Two traps: the array is allowed to contain
  **holes** (`Current` is a high-water mark, not a count), so compact before use; and
  `agiLighter::ACTIVELIGHTS` is a per-object scratch cache written only by the still-closed assembly
  CPU-lighting machinery. A path that bypasses CPU lighting never has it refreshed, so it can hold
  **dangling pointers** from lights destroyed since the last CPU-lit draw. This crashed with an access
  violation after a race reset tore down its lights. Use `LIGHTS`, which is maintained by
  reimplemented C++ (`DeclareLight()`, called from each `agiLight` constructor) and stays valid.

### Lightning

The original lightning is **sky-only**: a thunder cue raises `mmSky::DoFlash`, and `mmSky::Draw` swaps
in a flash texture for a single frame and clears the flag. It never lights the city.

To make it flood the scene it must be **latched before any drawing**, because the sky is drawn first
and consumes the flag — by the time city geometry renders it is already gone. Hold it with a decay
over several frames and feed it as a broad sky-dominant irradiance burst, which is what a strike
optically is, rather than as a directional light.

`mmSky::IsFlashing()` exists as an accessor because MSVC encodes access level into the mangled name
(`?DoFlash@mmSky@@0HA` private vs `@2HA` public), so widening the access of an imported static changes
the symbol being linked against and breaks the import.

---

## 4. Materials — there is no PBR data, and that is fine

MM1 ships **colour maps only**. No roughness, metalness or normal data exists anywhere in the original
assets. Pathway B's answer was not to invent values but to derive them from `agiTexProp`, the engine's
own per-texture semantic flags (`agiworld/texsheet.h`): road/floor surfaces, worn surfaces,
transparent surfaces, alpha-keyed foliage, and content explicitly marked emissive or not-to-be-lit.

That is real artist intent rather than guesswork, it needs no new assets, and it is directly useful to
Remix material config. `agiTexProp::AlphaGlow` in particular is **emissive by definition** — it is
also the flag the whole light harvest keys off.

`agiTexDef::Tex.Name` is the natural lookup key, and the engine already does variant lookups on it
(`TexSearchPath = "tex16a\0tex16o\0tex16\0"`), so companion textures (`<name>_n`, `<name>_orm`,
`<name>_e`) slot into a mechanism that exists.

### Vehicle reflection

- `agiNativeReflectivity` — 0 for ordinary geometry, 1 for a vehicle body. Set by
  `agiMeshSet::DrawLitSph`.
- `agiNativeReflectionTex` — the vehicle's own authored sphere map for the draw in flight, or null.

Also relevant: an analytic environment probe beat a captured one here. Six extra scene renders per
frame is not affordable on a backend with no frustum culling, and the result would be dominated by sky
anyway — a car sees sky above the horizon, road below it, and buildings only in a thin band. Computing
it from the same sun/sky/ground the rest of the frame uses costs a fraction of a millisecond, only
when the inputs change, and agrees with the direct lighting by construction. Roughness came from
generating each mip independently with a progressively broader sun lobe rather than filtering across
cube faces, which is what prefiltering an analytic environment converges to anyway.

---

## 5. Data quality bounds everything

Mesh normals are stored as an index into a **198-entry direction table**, roughly 26° apart. That is
coarse enough that all three corners of a facet often quantise to the *same* normal — and
interpolating a constant gives a constant, so low-poly bodywork shades flat no matter how good the
lighting is. No shading model recovers a gradient from constant input.

This matters for Remix too: replacement meshes will not have this problem, but anything computed from
original normals will.

---

## 6. Lifetime and device facts worth carrying forward

These bit hard and are not obvious from reading the code.

**The engine does not reset the device to change resolution — it destroys the entire graphics pipeline
and builds a new one.** Going from the 640×480 menu into a race runs `~agiDX9Context` (releasing the
`IDirect3D9`) and immediately asks for another. Consequences:

- Releasing the last reference to the D3D9 *module* across that gap is fatal under the Remix bridge:
  the server logs "D3D9 Module destroyed", tears the runtime down, and faults on the next
  `Direct3DCreate9`. One permanent reference held on the module fixes it.
- Anything holding device-derived state across that boundary — an overlay, a cached texture, a light
  registry holding `agiTexDef*` — is holding garbage afterwards. This is the leading suspect for
  external tools that work in the showroom and stop working in gameplay, because the showroom runs on
  device #1 and gameplay on device #2.

**Remix stops path tracing after the first "UI" draw of a frame, and this engine manufactures fake
UI.** dxvk-remix (`d3d9_rtx.cpp`, `makeDrawCallType`) classifies a fixed-function draw as UI when the
projection looks orthographic (`_44 == 1.0f`; identity qualifies) and `D3DRS_ZWRITEENABLE` is off,
gated on `rtx.orthographicIsUI` (default true). The first such draw latches RTX injection and every
draw after it that frame is rasterized only — and the game still looks normal, because
`rtx.skipDrawCallsPostRTXInjection` defaults false.

Every in-scene screen-space effect here (blob shadows, glow cards, smoke) is RHW with zwrite off. The
census measures the consequence exactly: **the showroom issues zero in-scene RHW draws and path traces
correctly; gameplay issues 12–27 every frame.** Zero-build way to confirm the mechanism:
`rtx.orthographicIsUI = False` in `rtx.conf`.

**Do not detect Remix by DLL name.** Remix's normal install is a drop-in named `d3d9.dll`, and
chaining proxies are also named `d3d9.dll` and load the Remix runtime themselves. A test for "remix"
in the requested name cannot fire in either case. Probe the loaded module for
`remixapi_InitializeLibrary` instead, and log the resolved path.

---

## 7. Diagnostics that earned their keep

- **The submission census**, every 120 frames: world vs screen triangle split, in-scene vs post-scene
  screen draws, static-lit vs unlit counts, live glow-light count. "world share (3D only)" is the
  number to watch. This is what proved the showroom/gameplay difference above, and no amount of
  looking at the screen could have.
- **`OPEN1560_NATIVE_MASK`** — environment variable, bitmask, selects which draw entry points may take
  the world path. Set it to 0 to restore original behaviour with no rebuild. Fastest way to answer "is
  this regression mine?".
- **Harvest accounting** — `CardsSeen` / `CardsNoTexture` / `CardsNotGlow` / `CardsHarvested`. These
  answer a question nothing visual can: when a lamp emits no light, is its billboard *not reaching*
  the hook, or reaching it and being *rejected*? Those are opposite bugs with identical symptoms.
- **Do not deduplicate a diagnostic on the wrong key.** `glowdebug` originally logged one line per
  texture. Street lamps and traffic signals share the `FXLTGLOW` sheet and a signal always drew first,
  so no street lamp ever printed a line — making "lamps are never harvested" and "lamps are harvested
  then dropped" look identical. Two wrong diagnoses came out of that before the key was widened to
  texture + hue. If a log is being used to rule something out, check what it *cannot* show.
- **Measure, do not eyeball.** Several wrong diagnoses came from judging renders visually. Reading
  exact pixel values from a diagnostic render settled in one step what hours of looking had got wrong.

---

## 8. The one-line summary

Everything above is a variation on the same theme: **this engine already knows where its lights are,
what colour they emit, what time of day it is and what its surfaces are made of — it just never says
so through the D3D9 API.** Recovering it from the draw stream and from engine state is what makes a
path-traced MM1 possible without authoring a single new asset, and none of that work depends on
Pathway B having existed.
