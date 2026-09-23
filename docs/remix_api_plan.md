# Open1560 - RTX Remix API plan

This is the plan for giving RTX Remix game data it cannot get from D3D9 draw calls, through the
Remix API. It covers what has been built, what the bridge allows, and the phases still to come.
[remix_api_data_sources.md](remix_api_data_sources.md) is the companion document: it records where in
the engine each kind of data lives. This one records how that data reaches the runtime.

The sun and moon are deliberately out of scope for now.

---

## 1. Where things stand

| Phase | What | State |
|---|---|---|
| 0 | The channel: connect, configure, lifetime, diagnostics | **Done** - needs an in-game check |
| 1 | Glow sprites as real lights: street lamps, traffic signals, tail/brake/reverse lamps | **Done** - needs in-game tuning |
| 2 | Validation and tuning pass | Next - needs a Remix install (section 6) |
| 3 | Headlights as shaped (spot) lights | Planned |
| 4 | Engine dynamic lights through the API instead of D3DLIGHT9 translation | Planned |
| 5 | The flare sprites themselves: what Remix should do with the glow cards | Planned |
| 6 | Lightning as a scene-wide light burst | Planned |
| 7 | Update lights in place, once a bridge forwards it | Waiting on Remix Plus's bridge |

Code: `code/midtown/agidx9/dx9remix.{h,cpp}`, with the harvest in `agiworld/meshrend.cpp`
(billboards) and `agidx9/dx9rsys.cpp` (glow meshes).

---

## 2. How the game reaches the runtime

The game is 32-bit. The Remix runtime is 64-bit only. So the game never talks to the runtime: it
talks to the **bridge client**, a 32-bit `d3d9.dll`, which serialises each call and sends it to the
64-bit bridge server over the same channel as the D3D9 commands. The server then calls the real
runtime.

```
 game (x86)                     bridge client (x86 d3d9.dll)        bridge server (x64)      Remix runtime
 ----------                     ----------------------------        -------------------      -------------
 agiDX9RemixApiInit()     ----> remixapi_InitializeLibrary   (fills the interface table locally)
 CreateLight(info)        ----> serialise, new bridge handle ---->  runtime CreateLight ---> external light map[hash]
 DrawLightInstance(h)     ----> queue                        ---->  runtime Draw...     ---> this frame's light list
 Present()                ----> queue                        ---->  Present             ---> frame ends, list cleared
```

Because API commands share the D3D9 command stream, they are applied **in order** with the draws.
That is what makes "submit the lights just before Present" correct.

Two bridges are supported:

| Bridge | Source read | API header |
|---|---|---|
| NVIDIA RTX Remix | [bridge-remix](https://github.com/NVIDIAGameWorks/bridge-remix) `7dbbd371` | 0.5.1 |
| Remix Plus | the `bridge` folder of [RemixProjGroup/dxvk-remix](https://github.com/RemixProjGroup/dxvk-remix) `9aab34bd` | 0.6.4 before 2026-06-28, 0.1000.0 since |

Remix Plus's own reference is its
[`docs/RemixApi.md`](https://github.com/RemixProjGroup/dxvk-remix/blob/main/docs/RemixApi.md). It
describes the API as the 64-bit runtime implements it. What follows is what a **32-bit** game gets
through the bridge, which is less.

### 2.1 Facts about the bridges, read from their source

Each comes from the bridge client (`src/client/remix_api.cpp`), server (`src/server/main.cpp`),
serialisers (`src/util/util_remixapi.*`) and the runtime (`rtx_remix_api.cpp`,
`rtx_light_manager.cpp`, and Remix Plus's `rtx_fork_light.cpp`). They shape the design, and each is
easy to trip over.

1. **It is opt-in.** Both bridges return `REMIXAPI_ERROR_CODE_NOT_INITIALIZED` from
   `remixapi_InitializeLibrary` unless `bridge.conf` has `exposeRemixApi = True`. The game logs
   exactly that fix when it happens.
2. **The bridge ignores the version we send, and copies out its interface table in its own
   layout.** Three layouts exist, and they disagree about where the light functions are:

   | Layout | Slots | `CreateLight` at |
   |---|---|---|
   | NVIDIA 0.5.1 | 21 | 7 |
   | Remix Plus 0.6.4 (before 2026-06-28) | 41 | 9 |
   | Remix Plus 0.1000.0 | 41 | 8 |

   Remix Plus inserted `CreateMeshBatched` and `CreateLightBatched` into the middle of the table,
   then moved `SetCameraMediumMaterial` in its 0.1000.0 "interface-struct realign". Reading the
   table with the wrong header calls the wrong function, and copying a 41-slot table into a
   21-slot struct overruns the stack. So `agiDX9RemixApiInit` hands the bridge a zeroed buffer
   bigger than any of them and identifies the layout **by which slots came back filled**. Each
   bridge fills a fixed set of named fields, and that set lands in different slots in each layout.
   The three patterns were checked by compiling each bridge's fill list against each header. An
   unrecognised pattern is refused, with the mask in the log, never guessed at.
3. **Structs are prefix-compatible.** The vendored header is Remix Plus 0.1000.0. `LightInfo` grew
   `isDynamic` and `ignoreViewModel` at its end, so an older bridge serialises only the part it
   knows. `LightInfoSphereEXT` is identical in all three. **Before using any other struct, diff it
   against the header of every bridge in the table.** Material structs in particular have changed.
4. **Only part of the interface is forwarded.** Both: `CreateMaterial`, `DestroyMaterial`,
   `CreateMesh`, `DestroyMesh`, `DrawInstance`, `CreateLight`, `DestroyLight`,
   `DrawLightInstance`, `SetConfigVariable`. Remix Plus adds `SetGameValue` and `GetGameValue`,
   which make a blocking round trip to the server.

   Remix Plus's `UpdateLightDefinition` and `AutoInstancePersistentLights` are listed in its API
   reference, and the 64-bit runtime implements them, but **its bridge only stubs them**: they
   log "not yet plumbed through the bridge" once and fail. The same goes for `GetUIState` and
   `SetUIState`. A 32-bit game cannot update a light in place or have its lights re-drawn
   automatically yet. If a later Remix Plus bridge forwards them, section 7 says what to change.

   Neither forwards `Startup`, `Shutdown`, `Present`, `SetupCamera`, `dxvk_CreateD3D9`,
   `dxvk_RegisterD3D9Device`, or picking. The camera needs nothing from us anyway: Remix already
   reconstructs it from the D3D9 view and projection.
5. **API calls bind to the most recently created D3D9 device.** Initialisation therefore happens in
   `agiDX9Pipeline::BeginGfx`, after the context exists. The device is parked, not destroyed,
   across pipeline restarts, so the binding survives going from the menu into a race.
6. **Every `CreateLight` mints a new bridge handle; only `DestroyLight` frees it.** The server keeps
   a map from bridge handle to runtime handle. So:
   - Updating a light by calling `CreateLight` again with the same hash would leak one server-side
     map entry per call: at 50 moving lights and 60 fps, 3,000 entries a second for the whole
     session. An update is therefore **destroy, then create**.
   - **The re-created light needs a new hash.** Remix Plus does not destroy immediately: it queues
     the erase, by hash, for the start of the next frame. A light re-created under the same hash
     would be wiped a frame later, and every moving light would go dark. Each light's hash carries
     a generation that is bumped on every re-send (`LightHash` in `dx9remix.cpp`), so the old
     light's erase never touches its successor, on either runtime.
   - The cost: each re-send is a new light to the runtime, so the denoiser starts it fresh. Moving
     lights are re-sent on every frame they move. If that shows up as shimmer on vehicle lights, see
     section 8.
7. **Lights must be drawn every frame.** Upstream clears its list of drawn API lights at the end of
   every frame. Remix Plus queues each draw and applies it at the start of the next frame, after
   that frame's erases. Drawing every live light every frame is right for both. A created light
   that is not drawn is kept but gives no light.
8. **`isDynamic` matters on Remix Plus.** A light marked static that has not changed for a while is
   put to sleep, to keep its denoiser history. Vehicle lamps are sent as dynamic, and so is any
   light that is moving; street lamps and signals are static. NVIDIA's bridge does not send the
   field.
9. **`CreateLight` through the bridge always reports success.** The server calls the runtime later
   and can only log a failure. A light the runtime rejected shows up in the bridge server log as
   `Invalid light handle` on its first draw.
10. **Volumetrics: Remix Plus only.** Its bridge forwards `volumetricRadianceScale`; NVIDIA's stops
    at the sphere's shaping, so there it arrives as 0 and the lights take no part in volumetric fog.
11. Both bridges also export `remixapi_RegisterCallbacks` (begin-scene, end-scene and present hooks).
    We do not need it: this engine owns its frame loop and knows where the frame ends.

---

## 3. Phase 0 - the channel (done)

`agiDX9RemixApiInit()` runs once per process, from the first `BeginGfx` with `-remixapi` on. It:

- looks up `remixapi_InitializeLibrary` on the module `Direct3DCreate9` came from, then on
  `d3d9_remix.dll` and `d3d9.dll`. The fallbacks cover a chaining proxy that loads the bridge
  behind itself.
- initialises, identifies the bridge's table layout (fact 2), and logs which bridge it found.
- sets `agiGlowHarvestEnabled`. The harvest, the registry's per-frame ageing and the per-texture
  glow colour grid all cost nothing until this is set.
- applies `-remixconfig`, a `|`-separated list of `rtx.conf` keys, through `SetConfigVariable`.
  This lets the game's own ini carry the Remix settings that belong to this game.

Lifetime:

| Event | What happens |
|---|---|
| `BeginGfx` | `agiDX9RemixApiInit()` (no-op after the first time) |
| `BeginFrame` | `agiUpdateGlowLights()` ages the registry, if the harvest is on |
| each glow draw | harvested into the registry (section 4.1) |
| `EndFrame`, before `Present` | `agiDX9RemixApiSubmitFrame()` |
| `EndGfx` | `agiDX9RemixApiReleaseAll()`, then `agiResetGlowLights()` |

`EndGfx` matters. Quitting a race frees the city, and its textures, when the memory arena is reset.
The runtime's copies of the lights have to go at the same time, or the last race's street lamps
light the menu.

State is held in fixed arrays, not standard containers, for the same reason: the arena reset would
free anything allocated through the engine's heap underneath a process-lifetime structure.

Diagnostics: a `DX9 REMIXAPI` census line every 120 frames (live lights, drawn per frame, created,
updated, destroyed, over budget, failed), `-remixapidebug` to log the first 64 lights, and the
existing `glowlights=` and `cards=` fields in `DX9 CENSUS`.

---

## 4. Phase 1 - glow lights (done)

### 4.1 Harvest

This restores the retired programmable path's harvest (commit `414e87b9` unwired it), unchanged
apart from the enable flag:

- **Billboards**, in `agiMeshSet::DrawCard`: street lamps, traffic signals, signage. The position is
  in model space and is transformed by `ViewParams().World`. The UV comes from
  `CurrentMeshCard.Frames[4 * frame]`, so a traffic signal samples red, amber or green correctly.
- **Glow meshes**, in `agiDX9Rasterizer::MeshWorld` via `HarvestWorldGlow`: vehicle head, tail,
  brake and reverse lamps. They are clustered per flare, so a car gets two tail lights rather than
  one on its centre line. Gated on `IsInScene()`, which keeps showroom glows out.

The registry (`agiworld/glowlight.h`) keeps each light across frames, matches a moving sprite to its
slot by predicted position, and fades a light out over 6 frames once its sprite stops being drawn.

**New: `agiGlowLight::Id`.** A slot's index is not an identity, because `agiUpdateGlowLights`
compacts the array every frame. Each new light gets an id that is never reused in the process, and
that id is what a Remix light is keyed on: `hash = "O1" tag | generation << 32 | id` (fact 6).

### 4.2 From registry entry to Remix light

Per live entry, in `ResolveGlow`:

1. `colour = tint x fade(age)`, then multiplied by the glow texture's own hue sampled at the flare's
   UV (`agiDX9TexDef::SampleGlowColor`).
2. Reclassify against that resolved colour. The kind (street lamp, traffic, vehicle, headlight,
   generic) decides the intensity multiplier and whether the kind is enabled at all.
3. `gain = remixlightpower x (reach / 6)^2 x intensity`. This is the same formula the programmable
   path was tuned with, so `lightlamp`, `lightvehicle` and the rest mean what they meant there.
4. **`radiance = colour x gain / (pi x r^2)`**, with `r = remixlightradius`. A sphere light's
   irradiance at distance d is `radiance x pi x r^2 / d^2`. The programmable path's was about
   `gain / d^2`, so the two agree at every distance, and brightness does not depend on `r`.
   `r` only changes shadow softness.

Each light is a **sphere light**, radius 0.15 by default, with no shaping. That is right for a
street lamp or a tail lamp, which is small, bright and throws in all directions.

### 4.3 Per-frame submission

1. Resolve every registry entry to a candidate.
2. Keep the brightest `remixmaxlights` (192). This bounds bridge traffic; it is not a quality knob.
3. Match candidates to existing Remix lights by glow id, through a fixed open-addressed index.
4. Destroy Remix lights no candidate claimed: expired, over budget, or a disabled kind.
5. Create new lights. Re-send existing ones only if they moved more than 2 cm, changed brightness
   by more than 3%, or `remixlightradius` changed. A parked car's tail lights and every street lamp
   are sent once and then only drawn.
6. Draw every live light.

The position is the last harvested one, never extrapolated. Submission runs after all of the
frame's draws, so a light still being drawn is current. Pushing a fading light along its old
velocity is what made lights fly off across the city in the programmable path.

### 4.4 Headlights are off by default

`FXLTCONE`, the headlight cone, is a large mesh whose centre sits metres ahead of the bonnet. As a
point light it lights the road from the wrong place. `-remixheadlights` sends it anyway, for
comparison. Phase 3 is the real fix.

---

## 5. Settings

All of these are command-line switches and `Open1560-Shaders.ini` keys (`[RemixAPI]` and the glow
sections). A file generated before this change has no `[RemixAPI]` section: add the lines by hand, or
delete the file to regenerate it.

| Key | Default | |
|---|---|---|
| `remixapi` | 0 | Master switch |
| `remixlightpower` | 1.5 | Overall brightness |
| `remixlightradius` | 0.15 | Emitter size (shadow softness only) |
| `remixmaxlights` | 192 | Per-frame budget |
| `remixheadlights` | 0 | Send headlight cones |
| `remixconfig` | - | `rtx.conf` keys, `key=value\|key=value` |
| `remixapidebug` | 0 | Log light creation |
| `glowstreetlamps`, `glowtrafficlights`, `glowvehiclelights`, `glowgenericlights`, `glowheadlights` | 1 | Which kinds emit |
| `lightlamp`, `lighttraffic`, `lightvehicle`, `lightgeneric`, `lighthead` | 10, 2, 1.25, 1, 0.05 | Per-kind brightness |
| `glowreachscale`, `glowreachmin` | 14, 20 | Flare size to reach; brightness goes with its square |

`remixconfig` is `|`-separated because the ini loader treats `;` as a comment, and `rtx.conf`
values themselves contain commas.

---

## 6. Phase 2 - validation (needs a Remix install)

Nothing here has run against a live runtime yet: this environment has no Windows build or GPU. In
order:

1. **Connects.** Add `exposeRemixApi = True` to `bridge.conf` and run with `-remixapi`. The log
   should name the bridge it found, e.g. `Remix API: connected through the Remix Plus bridge (API
   0.1000.0)`. If it says the bridge refused, the conf line is not being read (check which `.trex`
   folder is in use). If it says `unrecognised bridge`, the bridge's table layout is not one of
   the three in fact 2: send the logged mask along with the Remix build, and the layout can be
   added.
2. **Harvest is live.** In a night race, `DX9 CENSUS` should show `glowlights=` well above 0 and
   `cards=... harvested` above 0. If `harvested` is 0 while `seen` is not, see the `CARD-NOTEX` and
   `CARD-NOTGLOW` notes in `remix_api_data_sources.md` §7 (`-glowdebug`).
3. **Lights reach the runtime.** `DX9 REMIXAPI` should show `live` steady on a still camera, and
   `created` falling to near 0 once the view settles. The bridge server log should have no
   `Invalid light handle` lines. In the Remix developer menu, the light debug view should show
   spheres sitting on the lamp heads.
4. **Positions are right.** Stand under a street lamp: the pool of light is under the lamp, not at
   the map origin (the §1.2 trap) and not offset. Watch a car's tail lights at speed: they stay on
   the lamps.
5. **Colours are right.** Traffic signals light the junction red, amber and green as they cycle.
   Tail lights are red, street lamps warm.
6. **Brightness.** Tune `remixlightpower` first, then the per-kind multipliers. Remix's exposure and
   tonemapper differ from the old shader's, so 1.5 is a starting point, not a target.
7. **Lifetime.** Quit to the menu: no light should survive into it. Start a second race: lights come
   back and `live` matches the first race.
8. **Cost.** Compare frame time with `remixapi` 0 and 1 at a busy junction. If the bridge is the
   bottleneck, lower `remixmaxlights`.

Worth deciding with those results: `rtx.fallbackLightMode=0` through `remixconfig`, if Remix's
fallback light is still switching on in scenes that now have lights of their own.

---

## 7. Later phases

### Phase 3 - headlights as spot lights

The cone mesh is the wrong data. What a headlight needs is the lamp position and the car's facing.
Both are available in `HarvestWorldGlow`, which receives the draw's `world` matrix:

- Harvest `FXLTCONE` separately: take the car's forward axis from `world` (the sign needs checking
  against a known heading), and put the light at the near end of the cone, where it meets the car,
  not at its centroid.
- Extend `agiGlowLight` with an optional direction, and send a sphere light with
  `shaping_hasvalue = 1`: direction, `coneAngleDegrees` about 30-40, some `coneSoftness`.
- Headlights are on only at night and in poor weather, which the engine already decides by drawing
  the cone or not. The registry's TTL handles the switch.

### Phase 4 - the engine's dynamic lights

`agiLighter::LIGHTS` are sent to the device as `D3DLIGHT9`s (`SetupD3D9Lights`), and Remix
translates those itself, with its own guesses about radius and falloff. Sending them through the API
instead gives direct control: point lights as spheres, spot lights with real shaping, sizes chosen
here. That only makes sense if Remix's own translation of fixed-function lights is turned off
through `remixconfig`, or each light counts twice. Check the key name against the runtime in use
before relying on it. The `LIGHTS` holes and dangling-pointer traps are in
`remix_api_data_sources.md` §3.

### Phase 5 - the flare sprites

With real lights in place, the glow cards (additive `AlphaGlow` quads) are still drawn. Under path
tracing they read either as flat glowing cards or as nothing. The options are Remix texture
categories (particle, ignore, or a light-converting category), set once in the Remix toolkit and
saved to `rtx.conf`. The engine cannot compute Remix's texture hashes itself, so this is a toolkit
step, not a code step. Remix Plus's runtime has `dxvk_GetTextureHash` and `AddTextureHash`, which
would let the game tag its own glow textures, but its bridge forwards neither yet. The thing to decide is whether the flare stays visible as a bloom-like
sprite or is removed and left to the light.

### Phase 6 - lightning

The original lightning only swaps the sky texture for one frame. `agiLightningFlash`
(`glowlight.h`) was a latched, decaying 0..1 value for exactly this. Its latch in
`mmCullCity::Cull()` was unwired with the programmable path. As an API light it is one
`LightInfoDistantEXT`, or a large dome light, created on a strike, re-sent as it decays, and
destroyed at 0. The latch has to be restored first, and the §3 note about why it must be latched
before the sky draws still applies.

### Phase 7 - update lights in place (Remix Plus)

Remix Plus's runtime has `UpdateLightDefinition` (change an existing light, keeping its identity)
and `AutoInstancePersistentLights` (keep drawing live lights without a call per light). Its bridge
stubs both today (fact 4). When a Remix Plus bridge forwards them:

- Add its table layout to `kBridgeLayouts` (its filled-slot mask changes the moment it fills a new
  field, so it is refused until then, which is the safe failure).
- For that layout, re-send a changed light with `UpdateLightDefinition` on its existing handle
  instead of destroy-and-create. The light keeps one hash and one identity, the denoiser keeps its
  history, and the generation in the hash stops changing.
- Optionally drop the per-light `DrawLightInstance` loop for one `AutoInstancePersistentLights`
  call, if persistent registration is also forwarded. Measure first: per-light draws are cheap.

### Possible later: weather through `SetGameValue` (Remix Plus)

Remix Plus's bridge does forward `SetGameValue`, and its sky system reads a `__weather.*`
convention (`docs/RemixSkyAPI.md` in Remix Plus). `MMSTATE.Weather` (sun, fog, rain, snow) could
drive it. This overlaps the sun and moon work, which is out of scope for now, and each call is a
blocking round trip to the server, so it would be set once per race, not per frame.

### Possible later: materials

`CreateMaterial` and `CreateMesh` are forwarded, but API materials only apply to API meshes. The
game's own D3D9 geometry gets its materials from Remix's texture-hash replacements (USD mods), so
there is nothing to do here unless the game starts sending meshes through the API. If it does,
remember fact 3: the material structs differ between the bridges' headers.

---

## 8. Risks and open questions

- **The API through the bridge is experimental** (NVIDIA's bridge's own words), and Remix Plus has
  already changed its table layout once. A bridge update that changes what it fills is refused at
  connect time rather than misread (fact 2), and the log says so.
- **Destroy-and-create on update** (fact 6) can make moving lights restart denoiser history. If
  vehicle lights shimmer, the options are: re-send moving lights every other frame, or phase 7 once
  Remix Plus's bridge forwards `UpdateLightDefinition`. Re-creating in place under the same hash is
  not an option: it leaks on both bridges, and on Remix Plus the queued erase would remove it.
- **Bridge traffic.** Each re-sent light is two messages plus its per-frame draw. The budget and
  change thresholds keep this bounded; section 6 step 8 measures it.
- **Threading.** The harvest writes the registry from the draw path. The engine has no render
  thread (the only other thread is the texture pager, which does not draw), and this is the same
  exposure the harvest had when it ran under the programmable path.
- **Glow textures loaded before the API connects** have no colour grid, and their lights fall back to
  the vertex tint. The API connects in the first `BeginGfx`, before any city texture exists, so this
  only affects the front end's own glows.
