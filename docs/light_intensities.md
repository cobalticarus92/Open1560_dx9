# Open1560 — light intensity reference

Every number that contributes to how bright a light is on Pathway B, where it lives, and what it was
measured at in-game. Written as a tuning worksheet: the values here are **not** claimed to be right,
they are claimed to be *what is currently there*, so that a tuning pass has a baseline to move from
rather than a search.

Nothing in this document is a defaults change.

**These values are currently inert.** The programmable path they feed is not wired up (see the note
in `agiDX9Pipeline::BeginGfx`), so nothing reads them at runtime. They are kept as the measured
baseline a tuning pass would start from if it is turned back on.

---

## 0. Read this first — the reach² trap

Emitted brightness is **not** the per-kind intensity multiplier. It is

```
emitted = tint x fade x texture_hue x (glowpower x reach_ref² x intensity)
                                       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                                                    gain
where reach_ref = max(reach, 1) / 6
```

`reach` is derived from the flare's *drawn size*, which varies from 20 (the floor) to ~45 across the
observed population. Because it is squared, that spread is a **5x swing in gain** — larger than most
of the per-kind multipliers. Tuning `lightlamp` while ignoring reach is chasing the smaller lever.

Worked out with current defaults (`d3d9glowpower = 1.5`):

| kind | intensity | observed reach | reach_ref² | **gain** |
|---|---|---|---|---|
| street lamp | 10.0 | 20.0 (floor) | 11.1 | **166.7** |
| street lamp *(user ini 25.0)* | 25.0 | 20.0 | 11.1 | **416.7** |
| traffic signal | 2.0 | 31.9 | 28.3 | **84.8** |
| vehicle tail/brake | 1.25 | 20.0 (floor) | 11.1 | **20.8** |
| generic neutral white | 1.0 | 20.0 (floor) | 11.1 | **16.7** |
| headlight cone | 0.05 | 44.7 | 55.5 | **4.2** |

Note a traffic signal at intensity 2.0 lands within 2x of a street lamp at 10.0, purely because its
flare is drawn 60% larger. That is the reach² term doing the work, and it is the first thing to
question in a tuning pass.

### What the gain means at the surface

Attenuation is windowed inverse-square with the denominator clamped at one world unit:

```
window = saturate(1 - dist² / reach²)
atten  = window² / max(dist², 1)
```

For a street lamp (gain 166.7, reach 20):

| distance | atten | gain x atten | verdict |
|---|---|---|---|
| 1 u | 1.000 | 166.7 | wildly overbright, relies entirely on ACES to roll off |
| 6 u | 0.023 | 3.8 | still clipping, but this is the pavement right under the lamp |
| 12 u | 0.0029 | 0.48 | the useful band |
| 18 u | 0.00026 | 0.04 | effectively gone |

So a lamp's visually meaningful range is roughly **8–15 units**, and everything inside ~7 units is
saturated. The one-unit denominator clamp is a stand-in for a finite source radius; a proper
sphere-area light would replace it, removing the near-field blowout and making these numbers behave
linearly.

---

## 1. Per-kind intensity — `agiClassifyGlowIntensity`

`code/midtown/agiworld/meshrend.cpp`. Evaluated in this order; first match wins.

| # | test | parameter | default | user ini |
|---|---|---|---|---|
| 1 | texture name contains `CONE` | `lighthead` | 2.0 (was 0.05 while headlights were point lights at the cone's centre) | — |
| 2 | name contains `FXLTGLOWRED` / `FXLTGLOWAMBER` | `lightvehicle` | 1.25 | 1.25 |
| 3 | peak channel <= 1e-4 (black) | `lightlamp` | 10.0 | — |
| 4 | relative saturation `(peak-floor)/peak > 0.65` | `lighttraffic` | 2.0 | 2.0 |
| 5 | warm: `blue/peak < 0.85` | `lightlamp` | 10.0 | **25.0** |
| 6 | otherwise (neutral white) | `lightgeneric` | 1.0 | *(absent)* |

**`lightlamp = 25.0` in the deployed ini is the highest-priority thing to revisit.** It was raised
while lamps were still landing at the world origin and contributing nothing, so it was compensating
for a bug that no longer exists. At 25.0 a lamp's gain is 416.7 and everything within ~10 units of it
clips. The built-in default of 10.0 is the more honest starting point.

The classifier is run **twice**: provisionally at harvest time against the vertex tint, then again by
the renderer against `tint x SampleGlowColor(u,v)` once the texture hue is known. `glowdebug` prints
the *provisional* value, so a line reading `intensity=1.00` may still resolve to a lamp downstream.

### Measured population

Everything observed in a night cruise, with `-glowdebug`:

| texture | tint | rel. sat | reach | class |
|---|---|---|---|---|
| `FXLTGLOW` | 0.98 0.94 0.63 | 0.36 | 20.0 | street lamp |
| `FXLTGLOW` | 0.99 0.98 0.50 | 0.49 | 20.0 | street lamp |
| `FXLTGLOW` | 1.00 0.98 0.47 | 0.53 | 20.0 | street lamp |
| `FXLTGLOW` | 0.00 1.00 0.44 | 1.00 | 31.9 | traffic signal (green) |
| `FXLTGLOW` | 0.99 0.00 0.00 | 1.00 | 31.9 | traffic signal (red) |
| `FXLTGLOW` | 0.98 0.73 0.00 | 1.00 | 31.9 | traffic signal (amber) |
| `FXLTGLOW` | 1.00 1.00 1.00 | 0.00 | 20.0 | generic neutral |
| `FXLTGLOWRED` | 0.50 0.50 0.50 | 0.00 | 20.0 | vehicle (by name) |
| `FXLTCONE` | 1.00 0.97 0.71 | 0.29 | 39.2–44.7 | headlight cone |
| `VPCOP_TOPLIGHT` | 0.22 0.31 1.00 | 0.78 | 20.0 | traffic-class (police roof) |

Note that all three street lamps and all three signals share the **same `FXLTGLOW` sheet** and are
separated only by colour. Any change to the saturation or warmth thresholds moves lamps and signals
against each other; they cannot be tuned independently by texture.

`VPCOP_TOPLIGHT` currently falls into the traffic-signal bucket at 2.0. It is a police roof bar, not
a signal. It is probably fine there, but it is a deliberate-looking result that was not deliberate.

---

## 2. Reach — `agiGlowLightReach`

```
reach = max(flare_half_extent x glowreachscale, glowreachmin)
```

| parameter | default | note |
|---|---|---|
| `glowreachscale` | 14.0 | flare half-extent -> world units |
| `glowreachmin` | 20.0 | floor; most lights hit this |
| `kMaxLightReach` | 128.0 | hard-coded clamp in `dx9shader.cpp`, applies to grid *and* shader |

Shared by both harvest routes (`agiAddGlowLight` for billboards, `HarvestWorldGlow` for glow meshes)
so identical fixtures get identical reach. They used to disagree 24 vs 14, a ~3x brightness error.

Because the floor is 20 and most flares are small, **the majority of lights in the city currently sit
at exactly reach 20** and therefore share one gain. Lowering `glowreachmin` would let the flare size
actually differentiate them — at the cost of small distant flares becoming lights that illuminate
nothing but themselves, which is what the floor exists to prevent.

---

## 3. Global scales

| parameter | default | file | effect |
|---|---|---|---|
| `d3d9glowpower` | 1.5 | `dx9shader.cpp` | master multiplier on every glow-derived light |
| `d3d9exposure` | 1.0 | `dx9shader.cpp` | applied to the whole frame before tonemapping |
| `d3d9tonemap` | 1 (on) | `dx9shader.cpp` | ACES; without it everything above clips flat white |
| `d3d9flashpower` | 4.0 | `dx9shader.cpp` | lightning burst added to hemisphere irradiance |
| `d3d9lightspec` | 1 (on) | `dx9shader.cpp` | Cook-Torrance specular from clustered point lights |

ACES is doing a lot of load-bearing work here. With the near-field values in §0 exceeding 1.0 by two
orders of magnitude, the curve's shoulder is the only thing preventing large white blobs. A tuning
pass that reduces the gains should re-check whether the curve is still earning its place.

---

## 4. Static rig and ambient

Not glow-derived; these come from the engine's own lighting and track time of day and weather.

| source | where |
|---|---|
| `agiMeshLighterSun` / `Fill1` / `Fill2` + their `*Color` | three analytic directionals, `c7..c12` |
| `agiMeshLighterAmbient` (static) / `agiLighter::SceneAmbient` (dynamic) | hemisphere base |

Ambient is squared into linear space, then split into two lobes:

```
sky    = ambient² x 1.35 + flash
ground = ambient² x 0.50 + flash x 0.33
```

The 1.35 / 0.50 split is a judgement call chosen to keep total energy near the flat ambient the CPU
rig applies while still giving surfaces directional shape. `g_Ambient.rgb` is uploaded as zero on
purpose — adding it on top of a hemisphere already built from it was double-counting.

Light colours are authored by the engine as **0..1 multipliers, not radiance**, which is why the
shader folds out Lambert's 1/PI (`INV_PI_COMPENSATED = 1.0`). Any future change to that constant
rescales the entire static rig relative to the point lights and must be done together with §1.

---

## 5. Material response

`agiDX9ResolveMaterial`, `dx9shader.cpp`. Derived from `agiTexProp` flags — real artist intent, since
MM1 ships colour maps only.

| condition | roughness | metalness | note |
|---|---|---|---|
| default | 0.75 | 0.0 | |
| vehicle (`!StaticLighting`) | 0.30 | 0.0 | dielectric lacquer, not metal |
| `RoadFloorCeiling` | 0.88 | 0.0 | asphalt, concrete |
| `DullOrDamaged` | max(_, 0.95) | 0.0 | |
| `Transparent` | 0.15 | 0.0 | glass |
| `Chromakey` | 0.95 | 0.0 | foliage, fences |
| `AlphaGlow` / `Lightmap` / `NotLit` | — | — | forced to the unlit permutation |

Vehicle `AoAmount` is 0.5, everything else 1.0. Nothing is metallic anywhere — deliberate, because
the environment term is a two-lobe hemisphere and a metal with nothing to reflect renders black.
This is the single biggest thing that real environment probes would unlock.

---

## 6. Lifetime, pooling and culling

| constant | value | file |
|---|---|---|
| `AGI_GLOW_LIGHT_TTL` | 12 frames | `glowlight.h` |
| `AGI_GLOW_LIGHT_FADE` | 6 frames | `glowlight.h` |
| `AGI_MAX_GLOW_LIGHTS` | 512 slots | `glowlight.h` |
| slot match radius | 0.9 m (`kMatchDistSq = 0.81`) | `meshrend.cpp` |
| pool capacity / uploaded | 512 / **256** | `dx9shader.cpp` |
| cluster grid | 32 x 8 x 32 = 8192 cells | `dx9shader.h` |
| cell size | 24.0 (`d3d9cellsize`) | `dx9shader.cpp` |
| lights per cell | 16 | `dx9shader.h` |

Pool overflow and cell overflow both drop the **lowest-emitted-energy** light, because insertion runs
in descending energy order. With reach² in the energy term, that ranking is dominated by big flares —
worth remembering if a tuning pass changes the reach curve.

Measured occupancy in a dense night scene: 119–143 live, 117–127 pooled, ~2,000 cell slots. Nowhere
near the 256 ceiling, so the cap is not currently a tuning constraint.

---

## 7. Suggested order for the tuning pass

1. **Reset `lightlamp` to 10.0** and re-judge. The 25.0 was compensating for the origin bug.
2. Decide whether reach² is the right exponent at all, or whether gain should be decoupled from flare
   size. This is the structural question and it dominates everything below it.
3. Re-balance signals against lamps, remembering they share a texture and are separated only by the
   0.65 saturation threshold and the 0.85 warmth threshold.
4. Revisit `glowreachmin = 20`, which is currently flattening most of the population onto one gain.
5. Only then touch `d3d9glowpower` and `d3d9exposure`, which are whole-frame levers and will mask
   whatever is unbalanced underneath.
6. Re-check whether ACES is still needed once the near-field values are sane.

Measure with `-glowdebug` and the per-frame census rather than by eye; sampling exact pixel values
from a render settled several questions in this work that hours of looking had got wrong.
