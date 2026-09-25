/*
    Open1560 - An Open Source Re-Implementation of Midtown Madness 1 Beta
    Copyright (C) 2020 Brick

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "dx9remix.h"

#include "agiworld/glowlight.h"
#include "agiworld/glowtune.h"

#include "dx9context.h"
#include "dx9texdef.h"

#include "dx9_windows.h"

#include "dx9remixapi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>

define_dummy_symbol(agidx9_dx9remix);

// Off by default. The bridge only exposes the API when bridge.conf opts in, and turning this on
// changes the picture - every street lamp and tail light starts lighting the scene - so it should be
// a decision, not something that happens because Remix was detected.
static mem::cmd_param PARAM_remixapi {
    "remixapi", "Send game lights to RTX Remix through the Remix API (needs exposeRemixApi = True in bridge.conf)"};

// Brightness, in the same units as the retired programmable path's -d3d9glowpower so the per-kind
// multipliers (lightlamp, lightvehicle, ...) keep meaning what they meant there.
static mem::cmd_param PARAM_remix_lightpower {"remixlightpower", "Brightness of Remix API glow lights"};

// Physical size of the emitter. The engine records a flare's drawn size, which is a corona, not a
// bulb; a sphere that big would read as a glowing ball. Brightness is independent of this (see
// ResolveGlow), so it only changes how soft the shadows are.
static mem::cmd_param PARAM_remix_lightradius {"remixlightradius", "Radius of a Remix API glow light, in world units"};

static mem::cmd_param PARAM_remix_maxlights {"remixmaxlights", "Most Remix API glow lights sent per frame"};

// Headlights are spot lights: put at the lamp and aimed down the beam the game draws (see
// HarvestHeadlightBeam, dx9rsys.cpp), and sent with Remix light shaping. On by default now that they
// light the road from the right place; they used to be off because a headlight was a point light at
// the centre of its beam mesh, metres down the road.
static mem::cmd_param PARAM_remix_headlights {"remixheadlights", "Send headlights as Remix API spot lights"};

// Default edge softness of a headlight beam, 0 (hard) to 1. The half-angle is measured off the beam
// mesh; both can be set per kind or texture in Open1560_RemixAPI.ini (cone, softness).
static mem::cmd_param PARAM_remix_beamsoftness {"remixbeamsoftness", "Edge softness of Remix API headlight beams"};

// rtx.conf overrides applied once the API connects, as key=value pairs separated by '|'. Lets the
// game's own ini carry Remix settings that belong to this game, e.g. rtx.fallbackLightMode=0 now
// that the scene has lights of its own.
//
// '|' rather than ';' or ',': the ini loader (dx9config.cpp) treats ';' as the start of a comment,
// and rtx.conf values themselves use commas (vectors, texture hash lists).
static mem::cmd_param PARAM_remix_config {"remixconfig", "Remix options applied through the API: key=value|key=value"};

static mem::cmd_param PARAM_remix_debug {"remixapidebug", "Log Remix API light creation"};

namespace
{
    // Every light this module creates has a hash made of three parts:
    //
    //   bits 48-63  a tag, "O1", which keeps ours recognisable in a Remix capture and away from 0
    //               (the runtime keys API lights by hash and rejects 0)
    //   bits 32-47  a generation, bumped each time the light is re-sent
    //   bits  0-31  the glow light's id (agiGlowLight::Id)
    //
    // The generation is what makes re-sending safe on every runtime. An update through the bridge is
    // a destroy and a create (see dx9remix.h), and Remix Plus does not destroy immediately: it queues
    // the erase for the start of the next frame, by hash. Re-creating under the SAME hash would have
    // that queued erase wipe the new light a frame later, and every moving light would go dark. A new
    // hash per generation is never touched by the old one's erase.
    constexpr u64 kHashTag = 0x4F31000000000000ull;

    u64 LightHash(u32 glow_id, u16 generation)
    {
        return kHashTag | (static_cast<u64>(generation) << 32) | glow_id;
    }

    constexpr f32 kPi = 3.14159265f;

    // Same ceiling the programmable path used (dx9shader.cpp, BuildLightPool). Reach is squared into
    // the brightness, so an unbounded one lets a single mis-sized flare outshine the city.
    constexpr f32 kMaxGlowReach = 128.0f;

    // Change thresholds for re-sending a light. Through the bridge an update is a destroy plus a
    // create (see dx9remix.h), so a street lamp that has not changed must not be re-sent every frame.
    // 2 cm is well under anything visible in a path-traced shadow; 3% is under what a viewer notices
    // in a light's brightness, and still lets a fade reach zero in its six frames.
    constexpr f32 kMoveEpsilonSq = 0.02f * 0.02f;

    // Per-frame movement above which a light not refreshed this frame is stale - about 3 m/s at 60
    // frames a second. See ResolveGlow.
    constexpr f32 kStaleMoveSq = 0.05f * 0.05f;
    constexpr f32 kRadianceEpsilon = 0.03f;

    // An aimed light is re-sent when its beam swings by more than about 1.5 degrees: well under what
    // shows at the far end of a beam, and above the frame-to-frame jitter of a car at rest.
    constexpr f32 kAimEpsilonCos = 0.99966f;

    // Light shaping for an aimed light. Aimed = false is a light that shines every way.
    struct Shaping
    {
        bool Aimed;
        Vector3 Direction;
        f32 Cone;
        f32 Softness;
    };

    // How many in-place updates a light takes before it moves to a new hash - see UpdateLight. At 60
    // updates a second, a light that moves every frame changes hash about every two seconds.
    constexpr u32 kMaxStaleHandles = 120;

    struct RemixLight
    {
        u32 GlowId; // 0 = free slot
        u16 Generation;
        remixapi_LightHandle Handle;

        // Bridge handles this light's current hash was created under before Handle, superseded by
        // in-place updates. Each still holds a map entry in the bridge server; they are destroyed
        // together when the hash is retired. See UpdateLight. One spare, for the handle that puts the
        // light out when its hash is retired.
        u16 StaleCount;
        remixapi_LightHandle Stale[kMaxStaleHandles + 1];

        Vector3 Position;
        Vector3 Radiance;
        f32 Radius;
        Shaping Shape;
        u32 SeenFrame;
    };

    struct Candidate
    {
        u32 GlowId;
        Vector3 Position;
        Vector3 Radiance;
        f32 Radius;
        Shaping Shape;
        f32 Power;
        bool Dynamic;
        i32 Slot;
    };

    // The functions this module calls, taken out of whichever interface table layout the bridge
    // filled in - see IdentifyBridge.
    struct RemixFunctions
    {
        PFN_remixapi_CreateLight CreateLight;
        PFN_remixapi_DestroyLight DestroyLight;
        PFN_remixapi_DrawLightInstance DrawLightInstance;
        PFN_remixapi_SetConfigVariable SetConfigVariable;

        // Remix Plus only (null on NVIDIA's bridge). Its sky and weather system reads game values
        // - see dx9remixsky.cpp.
        PFN_remixapi_SetGameValue SetGameValue;
    };

    constexpr u32 kMaxLights = AGI_MAX_GLOW_LIGHTS;

    // glow id -> slot, rebuilt each frame. Open addressing at under 50% load, fixed storage.
    //
    // Fixed arrays throughout, not std containers, and not by taste: this state lives for the whole
    // process, while the engine resets its memory arena when a race ends. Anything allocated through
    // the engine's heap and kept here would be freed underneath us at that point - the same hazard
    // that made the glow registry itself need agiResetGlowLights().
    constexpr u32 kIndexSize = 1024;
    static_assert((kIndexSize & (kIndexSize - 1)) == 0, "kIndexSize must be a power of two");
    static_assert(kIndexSize >= kMaxLights * 2, "keep the index under 50% load");

    struct Stats
    {
        u32 Frames;
        u32 Drawn;
        u32 Created;
        u32 Updated;
        u32 Destroyed;
        u32 Culled;
        u32 Failed;
        u32 Rehashed;
    };

    RemixFunctions s_remix {};
    agiDX9RemixSceneApi s_scene {};
    bool s_active = false;

    // Whether the runtime updates a light in place when CreateLight is called again with its hash.
    // See UpdateLight.
    bool s_update_in_place = false;
    bool s_init_tried = false;

    RemixLight s_lights[kMaxLights] {};
    u16 s_index[kIndexSize] {};
    Candidate s_candidates[AGI_MAX_GLOW_LIGHTS] {};
    u32 s_live = 0;
    u32 s_frame = 0;
    u32 s_debug_logged = 0;

    Stats s_stats {};
} // namespace

static u32 IndexHash(u32 glow_id)
{
    return (glow_id * 2654435761u) & (kIndexSize - 1);
}

static void RebuildIndex()
{
    std::memset(s_index, 0, sizeof(s_index));

    for (u32 i = 0; i < kMaxLights; ++i)
    {
        if (s_lights[i].GlowId == 0)
            continue;

        u32 h = IndexHash(s_lights[i].GlowId);

        while (s_index[h] != 0)
            h = (h + 1) & (kIndexSize - 1);

        s_index[h] = static_cast<u16>(i + 1);
    }
}

static i32 FindSlot(u32 glow_id)
{
    for (u32 h = IndexHash(glow_id);; h = (h + 1) & (kIndexSize - 1))
    {
        const u16 entry = s_index[h];

        if (entry == 0)
            return -1;

        if (s_lights[entry - 1].GlowId == glow_id)
            return static_cast<i32>(entry - 1);
    }
}

static bool IsFinite(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// A glow registry entry to a light's position and radiance, or false if it emits nothing.
//
// The colour and intensity follow the programmable path's BuildLightPool (dx9shader.cpp) exactly,
// because that is where they were tuned against real scenes: the harvested tint, faded by age,
// times the flare's own hue sampled out of the glow texture, then reclassified against that
// resolved colour - which is the first point that knows whether a white-tinted flare on an amber
// sheet is really a warm street lamp.
//
// What changes is the last step. The old path fed a windowed inverse-square falloff, where
// brightness at distance d is roughly gain / d^2. Remix's sphere light is physical: at distance d
// its irradiance is radiance * pi * r^2 / d^2. Setting radiance = gain / (pi * r^2) makes the two
// agree at every distance, and makes brightness independent of the radius chosen - which is why
// -remixlightradius only changes shadow softness.
static bool ResolveGlow(const agiGlowLight& glow, f32 radius, Candidate& out)
{
    if (glow.Id == 0)
        return false;

    const f32 fade = agiGlowLightFade(glow.Age);

    if (fade <= 0.0f)
        return false;

    // A MOVING light whose sprite was not drawn this frame is not sent. Its position is where the
    // lamp was, not where it is: the registry keeps such a slot alive for a few frames to ride out a
    // cull or LOD hiccup, which is right for a street lamp, but for a car at speed it is a light left
    // hanging in the air behind it. Dropping it for the frame costs a flicker at worst; sending it
    // is visibly wrong. Age 0 means refreshed this frame - submission runs after every draw.
    if ((glow.Age > 0) && (glow.Velocity.Mag2() > kStaleMoveSq))
        return false;

    Vector3 color = glow.Tint * fade;
    f32 intensity = glow.Intensity;

    const char* name = glow.Texture ? glow.Texture->Tex.Name : nullptr;

    if (glow.Texture)
    {
        // Every agiTexDef this pipeline creates is an agiDX9TexDef (agiDX9Pipeline::CreateTexDef),
        // and the registry is emptied at EndGfx, before any texture from another pipeline exists.
        const agiDX9TexDef* tex = static_cast<const agiDX9TexDef*>(glow.Texture);

        if (tex->HasGlowColors())
        {
            const Vector3 hue = tex->SampleGlowColor(glow.U, glow.V);
            color = {color.x * hue.x, color.y * hue.y, color.z * hue.z};
            intensity = agiClassifyGlowIntensity(name, color);
        }
    }

    const agiGlowKind kind = agiClassifyGlowKind(name, color);

    if (!agiGlowKindEnabled(kind))
        return false;

    if ((kind == agiGlowKind::Headlight) && !PARAM_remix_headlights.get_or(true))
        return false;

    // Hand tuning from Open1560_RemixAPI.ini (agiworld/glowtune.h), resolved against the kind as it
    // stands NOW, after the texture's hue has been sampled. The per-kind brightness is already in
    // `intensity` (it is the -light* switches); what is left is what a per-texture section sets, and
    // the colour and sphere size, which have no switch of their own.
    const agiGlowTuning tuning = agiResolveGlowTuning(name, kind);

    if (tuning.HasEnabled && !tuning.Enabled)
        return false;

    if (tuning.HasIntensity)
        intensity = std::max(tuning.Intensity, 0.0f);

    if (tuning.HasColor)
        color = {color.x * tuning.Color.x, color.y * tuning.Color.y, color.z * tuning.Color.z};

    const f32 light_radius = tuning.HasRadius ? std::max(tuning.Radius, 0.01f) : radius;

    const f32 reach = std::clamp(glow.Radius, 1.0f, kMaxGlowReach);
    const f32 reach_ref = reach / 6.0f;
    const f32 gain = PARAM_remix_lightpower.get_or(1.5f) * reach_ref * reach_ref * intensity;

    // Brightness is independent of the sphere's size by construction (see the note above), so a
    // per-texture radius changes only how soft that lamp's shadows are.
    const Vector3 radiance = color * (gain / (kPi * light_radius * light_radius));

    if (!IsFinite(radiance) || !IsFinite(glow.Position))
        return false;

    const f32 power = std::max(radiance.x, 0.0f) + std::max(radiance.y, 0.0f) + std::max(radiance.z, 0.0f);

    if (power <= 1e-3f)
        return false;

    out.GlowId = glow.Id;

    // The position as last harvested, never extrapolated. This runs at the end of the frame, after
    // every sprite drawn this frame has refreshed its slot, so a live light is current by
    // construction. A slot whose sprite has stopped drawing is fading out, and pushing it along its
    // old velocity is what made lights fly off across the city in the programmable path.
    out.Position = glow.Position;
    out.Radiance = {std::max(radiance.x, 0.0f), std::max(radiance.y, 0.0f), std::max(radiance.z, 0.0f)};
    out.Radius = light_radius;
    out.Power = power;
    out.Shape = {};

    // An aimed light. Remix keeps the radiance of a shaped light as it is and only masks it outside
    // the cone, so the brightness down the beam is exactly what it would be unshaped: the gain above
    // holds for headlights too, with the whole of it pointed down the road.
    if (glow.Direction.Mag2() > 0.5f)
    {
        const f32 cone = tuning.HasCone ? tuning.Cone : glow.ConeAngle;
        const f32 softness = tuning.HasSoftness ? tuning.Softness : PARAM_remix_beamsoftness.get_or(0.3f);

        out.Shape.Aimed = true;
        out.Shape.Direction = glow.Direction * (1.0f / std::sqrt(glow.Direction.Mag2()));
        out.Shape.Cone = std::clamp(cone, 1.0f, 179.0f);
        out.Shape.Softness = std::clamp(softness, 0.0f, 1.0f);
    }

    // Vehicle lamps are dynamic even when the car is parked: it can pull away at any moment. Anything
    // else is dynamic only while it is actually moving. Remix Plus uses this to decide whether a light
    // may be put to sleep - a static light that has not changed for a while stops being updated, to
    // keep its denoiser history. Other runtimes ignore the field.
    out.Dynamic =
        (kind == agiGlowKind::Vehicle) || (kind == agiGlowKind::Headlight) || (glow.Velocity.Mag2() > kMoveEpsilonSq);
    out.Slot = -1;

    return true;
}

static bool CreateLight(const Candidate& candidate, u16 generation, remixapi_LightHandle& out_handle)
{
    const f32 radius = candidate.Radius;
    const Vector3& position = candidate.Position;
    const Vector3& radiance = candidate.Radiance;

    remixapi_LightInfoSphereEXT sphere {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position = {position.x, position.y, position.z};
    sphere.radius = radius;
    sphere.shaping_hasvalue = 0;
    sphere.shaping_value = {};

    if (candidate.Shape.Aimed)
    {
        const Vector3& direction = candidate.Shape.Direction;

        sphere.shaping_hasvalue = 1;
        sphere.shaping_value.direction = {direction.x, direction.y, direction.z};
        sphere.shaping_value.coneAngleDegrees = candidate.Shape.Cone;
        sphere.shaping_value.coneSoftness = candidate.Shape.Softness;
        sphere.shaping_value.focusExponent = 0.0f;
    }

    // The Remix Plus bridge forwards this; NVIDIA's bridge stops at shaping_value, so there the
    // runtime sees 0 and these lights take no part in volumetrics.
    sphere.volumetricRadianceScale = 1.0f;

    // remixapi_LightInfo grew isDynamic and ignoreViewModel after NVIDIA's 0.5.1, at the END of the
    // struct. A bridge built against the older header serialises only the prefix it knows, so this
    // one layout is right for every bridge IdentifyBridge accepts.
    remixapi_LightInfo info {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = LightHash(candidate.GlowId, generation);
    info.radiance = {radiance.x, radiance.y, radiance.z};
    info.isDynamic = candidate.Dynamic ? 1u : 0u;
    info.ignoreViewModel = 0;

    out_handle = nullptr;

    // Through the bridge this reports only whether the command was queued: the server calls the
    // runtime later and can only log a failure. A light the runtime rejected then shows up in the
    // bridge server log as "Invalid light handle" on its first draw.
    if ((s_remix.CreateLight(&info, &out_handle) != REMIXAPI_ERROR_CODE_SUCCESS) || !out_handle)
    {
        ++s_stats.Failed;
        return false;
    }

    if (PARAM_remix_debug.get_or(false) && (s_debug_logged < 64))
    {
        ++s_debug_logged;
        Displayf("REMIXAPI: light %08X gen=%u pos=(%.1f %.1f %.1f) radiance=(%.1f %.1f %.1f) r=%.2f%s",
            candidate.GlowId, static_cast<u32>(generation), position.x, position.y, position.z, radiance.x, radiance.y,
            radiance.z, radius, candidate.Dynamic ? " dynamic" : "");

        if (candidate.Shape.Aimed)
        {
            const Vector3& direction = candidate.Shape.Direction;

            Displayf("REMIXAPI:   aimed (%.2f %.2f %.2f) cone=%.1f softness=%.2f", direction.x, direction.y,
                direction.z, candidate.Shape.Cone, candidate.Shape.Softness);
        }
    }

    return true;
}

// Destroys every bridge handle the light holds. They all name the same runtime light (its current
// hash), so the runtime erases it once and ignores the repeats; each destroy is what frees that
// handle's entry in the bridge server.
static void DestroyHandles(RemixLight& light)
{
    for (u32 i = 0; i < light.StaleCount; ++i)
        s_remix.DestroyLight(light.Stale[i]);

    light.StaleCount = 0;

    if (light.Handle)
        s_remix.DestroyLight(light.Handle);

    light.Handle = nullptr;
}

static void DestroySlot(RemixLight& light)
{
    DestroyHandles(light);

    light = {};
    --s_live;
    ++s_stats.Destroyed;
}

// Re-sends a light whose position, colour, size or aim changed. False if it could not be sent.
//
// WHY THIS IS NOT JUST "DESTROY AND CREATE"
//
// Through Remix Plus's bridge, a create and a destroy do not land in the same frame. CreateLight
// takes effect at once, and also registers the light as persistent: the runtime then lights it every
// frame by itself, drawn or not. DestroyLight is queued until Present and applied at the start of the
// frame after. So replacing a light every time it moved - destroy the old, create a successor under
// a new hash - kept the old one lit for a frame beside the new: every moving car trailed a copy of
// its lights one frame behind, which is exactly the lag that was reported. And every successor was a
// new light to the denoiser, with no history, so it faded in over several frames on top of that.
//
// Remix Plus updates a light in place instead when CreateLight is called again with a hash it
// already has (LightManager::addExternalLight), immediately, and without losing its temporal data.
// That is what happens here: same hash, new definition, no destroy.
//
// The bridge still mints a new handle for that call, and its server keeps a small map entry per
// handle until the handle is destroyed - which cannot happen while the light lives, because every
// handle of a light names the same runtime light, and destroying any of them erases it. So the
// superseded handles are collected, and after kMaxStaleHandles updates the light moves to its next
// generation's hash and the old one is released whole. Before that release, the old light is
// updated in place to (next to) no light at all, so the frame its erase is still queued shows no
// ghost either. Once every couple of seconds of driving, not once a frame.
//
// NVIDIA's runtime erases on destroy and does not replace a light on a repeated hash, so there the
// light is replaced under the next generation's hash every time, as it always was.
static bool UpdateLight(RemixLight& light, const Candidate& candidate)
{
    if (s_update_in_place && light.Handle && (light.StaleCount < kMaxStaleHandles))
    {
        remixapi_LightHandle handle = nullptr;

        if (!CreateLight(candidate, light.Generation, handle))
            return false;

        light.Stale[light.StaleCount++] = light.Handle;
        light.Handle = handle;

        return true;
    }

    if (s_update_in_place && light.Handle)
    {
        // Put the retiring light out first, in place and at once, so the frame it stays queued for
        // erase lights nothing. Not zero: a black light is not a valid light definition.
        Candidate dark = candidate;
        dark.Position = light.Position;
        dark.Radius = light.Radius;
        dark.Shape = light.Shape;
        dark.Radiance = {1e-6f, 1e-6f, 1e-6f};

        remixapi_LightHandle handle = nullptr;

        if (CreateLight(dark, light.Generation, handle))
        {
            light.Stale[light.StaleCount++] = light.Handle;
            light.Handle = handle;
        }

        ++s_stats.Rehashed;
    }

    // Retire the current hash and create the light's successor under the next one - see LightHash
    // for why the hash must change. The destroys are by the old hash, so they cannot touch the new
    // light whichever runtime applies them, or when.
    DestroyHandles(light);
    ++light.Generation;

    return CreateLight(candidate, light.Generation, light.Handle);
}

static void ApplyConfigOverrides()
{
    const char* overrides = PARAM_remix_config.value();

    if (!overrides || !*overrides)
        return;

    char buffer[1024];
    arts_strncpy(buffer, overrides, ARTS_TRUNCATE);

    for (char* entry = buffer; entry && *entry;)
    {
        char* next = std::strchr(entry, '|');

        if (next)
            *next++ = '\0';

        if (char* separator = std::strchr(entry, '='))
        {
            *separator = '\0';

            const char* key = entry;
            const char* value = separator + 1;

            while (*key == ' ')
                ++key;

            if (*key)
            {
                if (agiDX9RemixApiSetConfig(key, value))
                    Displayf("Remix API: %s = %s", key, value);
                else
                    Warningf("Remix API: could not set %s", key);
            }
        }

        entry = next;
    }
}

// WHICH BRIDGE, AND WHERE IT PUT ITS FUNCTIONS
//
// remixapi_InitializeLibrary copies out a remixapi_Interface laid out by the header the BRIDGE was
// built against. Neither bridge looks at the version we pass, and three layouts are in circulation
// that disagree about where the light functions are:
//
//   NVIDIA bridge-remix, API 0.5.1      21 slots. CreateLight is slot 7.
//   Remix Plus before 2026-06-28, 0.6.4 41 slots. Inserts CreateMeshBatched and CreateLightBatched,
//                                       and has SetCameraMediumMaterial before DrawInstance.
//                                       CreateLight is slot 9.
//   Remix Plus 0.1000.0 (vendored here) 41 slots. SetCameraMediumMaterial moved after Present
//                                       ("interface-struct realign", Remix Plus 2d6bde57).
//                                       CreateLight is slot 8.
//
// Reading the table with the wrong header calls the wrong function: with this header on NVIDIA's
// bridge, "CreateLight" is DestroyLight. And copying a 41-slot table into a 21-slot struct overruns
// the stack. So the bridge fills a zeroed buffer bigger than any of them, and the pattern of slots it
// filled identifies the layout exactly. Each bridge fills a fixed set of named fields (read from
// their src/client/remix_api.cpp), and those land in a different set of slots in each layout.
//
// A bridge built on the current layout that forwards MORE of it is accepted as that layout - see the
// end of IdentifyBridge. Anything else is refused rather than guessed at, and the log line gives its
// filled-slot mask so its layout can be added below.
namespace
{
    using AnyFn = void(REMIXAPI_PTR*)();

    constexpr u32 kMaxSlots = 64;

    struct InterfaceBuffer
    {
        remixapi_Interface Interface;
        AnyFn Slack[16];
    };

    static_assert(sizeof(InterfaceBuffer) <= sizeof(AnyFn) * kMaxSlots, "raise kMaxSlots");
    static_assert(sizeof(InterfaceBuffer) % sizeof(AnyFn) == 0, "remixapi_Interface is not all function pointers");

    // The slots compared: every slot any known bridge can write.
    constexpr u32 kFilledSlotCount = static_cast<u32>(sizeof(remixapi_Interface) / sizeof(AnyFn));
    static_assert(kFilledSlotCount <= 64, "the filled-slot mask is a u64");

    constexpr u64 SlotMask(std::initializer_list<u32> slots)
    {
        u64 mask = 0;

        for (u32 slot : slots)
            mask |= 1ull << slot;

        return mask;
    }

    constexpr u32 kNoSlot = 0xFFFFFFFF;

    struct BridgeLayout
    {
        const char* Name;
        u64 FilledSlots;
        u32 CreateLight;
        u32 DestroyLight;
        u32 DrawLightInstance;
        u32 SetConfigVariable;
        u32 SetGameValue; // kNoSlot where the bridge has none

        // The scene half: materials, meshes, instances. Every known bridge forwards these.
        u32 CreateMaterial;
        u32 DestroyMaterial;
        u32 CreateMesh;
        u32 DestroyMesh;
        u32 DrawInstance;

        // The runtime behind this bridge updates a light in place when CreateLight is called again
        // with its hash. See UpdateLight.
        bool UpdateInPlace;
    };

    constexpr BridgeLayout kBridgeLayouts[] {
        {"NVIDIA RTX Remix bridge (API 0.5.1)", SlotMask({1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12}), 7, 8, 9, 10, kNoSlot,
            false, 1, 2, 3, 4, 6},
        {"Remix Plus bridge (API 0.6.4, a build from before 2026-06-28)",
            SlotMask({1, 2, 3, 5, 8, 9, 11, 12, 13, 18, 19, 30, 31, 33, 34, 36, 40}), 9, 11, 12, 13, 36, false, 1, 2, 3,
            5, 8},
        {"Remix Plus bridge (API 0.1000.0)", SlotMask({1, 2, 3, 5, 7, 8, 10, 11, 12, 17, 18, 30, 31, 33, 34, 36, 40}),
            8, 10, 11, 12, 36, true, 1, 2, 3, 5, 7},
    };

    // The last entry must describe the vendored header itself, and does.
    static_assert(offsetof(remixapi_Interface, CreateLight) == 8 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, DestroyLight) == 10 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, DrawLightInstance) == 11 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, SetConfigVariable) == 12 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, SetGameValue) == 36 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, GetGameValue) == 40 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, CreateMaterial) == 1 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, DestroyMaterial) == 2 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, CreateMesh) == 3 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, DestroyMesh) == 5 * sizeof(AnyFn), "remix_c.h layout changed");
    static_assert(offsetof(remixapi_Interface, DrawInstance) == 7 * sizeof(AnyFn), "remix_c.h layout changed");
} // namespace

static const BridgeLayout* IdentifyBridge(const AnyFn (&slots)[kMaxSlots])
{
    u64 filled = 0;

    for (u32 i = 0; i < kFilledSlotCount; ++i)
        filled |= slots[i] ? (1ull << i) : 0ull;

    // Nothing may have been written past the largest known table either.
    for (u32 i = kFilledSlotCount; i < kMaxSlots; ++i)
    {
        if (slots[i])
            return nullptr;
    }

    for (const BridgeLayout& layout : kBridgeLayouts)
    {
        if (layout.FilledSlots == filled)
            return &layout;
    }

    // A newer build of the current layout: Remix Plus keeps adding bridge forwarding for functions its
    // header already declares (texture creation, first seen as slots 15 and 16), which fills more of
    // the same table without moving anything. So for the layout of the vendored header alone, every
    // slot it fills must still be filled and anything extra is accepted. A bridge that INSERTED a
    // function instead would shift every slot after it, which breaks this pattern and is refused.
    const BridgeLayout& current = kBridgeLayouts[ARTS_SIZE(kBridgeLayouts) - 1];

    if ((filled & current.FilledSlots) == current.FilledSlots)
        return &current;

    return nullptr;
}

void agiDX9RemixApiInit()
{
    if (s_init_tried)
        return;

    s_init_tried = true;

    // Off is the default, so say so: otherwise a game running under Remix with the API switched off
    // looks exactly like one where the API is broken - no lights, no sky, and nothing in the log.
    if (!PARAM_remixapi.get_or(false))
    {
        Displayf("Remix API: off. Set remixapi = 1 in the [RemixAPI] section of Open1560_RemixAPI.ini (or pass "
                 "-remixapi) to send lights and drive the Remix Plus sky.");
        return;
    }

    // The module Direct3DCreate9 came from first. Then the names the bridge client can be loaded
    // under, which covers a chaining proxy: a d3d9.dll that loads Remix behind itself has no Remix
    // exports of its own, but the bridge client it loaded is still in the process.
    HMODULE candidates[] {
        static_cast<HMODULE>(agiDX9D3D9Module()),
        GetModuleHandleA("d3d9_remix.dll"),
        GetModuleHandleA("d3d9.dll"),
    };

    PFN_remixapi_InitializeLibrary initialize = nullptr;

    for (HMODULE module : candidates)
    {
        if (!module)
            continue;

        initialize =
            reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(module, "remixapi_InitializeLibrary"));

        if (initialize)
            break;
    }

    if (!initialize)
    {
        Warningf("Remix API: no loaded D3D9 module exports remixapi_InitializeLibrary. -remixapi needs an RTX Remix "
                 "bridge d3d9.dll (NVIDIA's or Remix Plus's); Remix API lights are off.");
        return;
    }

    remixapi_InitializeLibraryInfo info {};
    info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    info.pNext = nullptr;
    info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);

    // Zeroed, and larger than any known table, so a bridge writes its whole table into it without
    // overrunning, and every slot it does not fill reads as null. See IdentifyBridge.
    InterfaceBuffer buffer {};
    const remixapi_ErrorCode status = initialize(&info, &buffer.Interface);

    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
    {
        // NOT_INITIALIZED is the bridge's answer when bridge.conf has not opted in. It is by far the
        // likeliest failure, and the fix is a one-line edit, so say exactly what it is.
        if (status == REMIXAPI_ERROR_CODE_NOT_INITIALIZED)
        {
            Warningf("Remix API: the bridge refused. Add `exposeRemixApi = True` to bridge.conf (in the .trex folder "
                     "next to the game's d3d9.dll) and restart; Remix API lights are off.");
        }
        else
        {
            Warningf("Remix API: remixapi_InitializeLibrary failed with %u; Remix API lights are off.",
                static_cast<u32>(status));
        }

        return;
    }

    AnyFn slots[kMaxSlots] {};
    std::memcpy(slots, &buffer, sizeof(buffer));

    const BridgeLayout* layout = IdentifyBridge(slots);

    if (!layout)
    {
        u64 filled = 0;

        for (u32 i = 0; i < kFilledSlotCount; ++i)
            filled |= slots[i] ? (1ull << i) : 0ull;

        Warningf("Remix API: unrecognised bridge (filled interface slots %08X%08X). Not calling it: a wrong guess "
                 "about its layout would call the wrong functions. Remix API lights are off - see "
                 "docs/remix_api_plan.md, section 2.",
            static_cast<u32>(filled >> 32), static_cast<u32>(filled));
        return;
    }

    s_remix.CreateLight = reinterpret_cast<PFN_remixapi_CreateLight>(slots[layout->CreateLight]);
    s_remix.DestroyLight = reinterpret_cast<PFN_remixapi_DestroyLight>(slots[layout->DestroyLight]);
    s_remix.DrawLightInstance = reinterpret_cast<PFN_remixapi_DrawLightInstance>(slots[layout->DrawLightInstance]);
    s_remix.SetConfigVariable = reinterpret_cast<PFN_remixapi_SetConfigVariable>(slots[layout->SetConfigVariable]);
    s_remix.SetGameValue = (layout->SetGameValue != kNoSlot)
        ? reinterpret_cast<PFN_remixapi_SetGameValue>(slots[layout->SetGameValue])
        : nullptr;
    s_update_in_place = layout->UpdateInPlace;

    s_scene.CreateMaterial = reinterpret_cast<PFN_remixapi_CreateMaterial>(slots[layout->CreateMaterial]);
    s_scene.DestroyMaterial = reinterpret_cast<PFN_remixapi_DestroyMaterial>(slots[layout->DestroyMaterial]);
    s_scene.CreateMesh = reinterpret_cast<PFN_remixapi_CreateMesh>(slots[layout->CreateMesh]);
    s_scene.DestroyMesh = reinterpret_cast<PFN_remixapi_DestroyMesh>(slots[layout->DestroyMesh]);
    s_scene.DrawInstance = reinterpret_cast<PFN_remixapi_DrawInstance>(slots[layout->DrawInstance]);
    s_active = true;

    // Only now start paying for the harvest: the glow registry, its per-frame ageing and each glow
    // texture's colour grid all exist for this consumer and cost nothing while it is absent.
    agiGlowHarvestEnabled = true;

    Displayf("Remix API: connected through the %s", layout->Name);

    u64 filled = 0;

    for (u32 i = 0; i < kFilledSlotCount; ++i)
        filled |= slots[i] ? (1ull << i) : 0ull;

    if (const u64 extra = filled & ~layout->FilledSlots)
    {
        Displayf("Remix API: this bridge also forwards interface slots %08X%08X, which this game does not use",
            static_cast<u32>(extra >> 32), static_cast<u32>(extra));
    }

    ApplyConfigOverrides();
}

bool agiDX9RemixApiActive()
{
    return s_active;
}

const agiDX9RemixSceneApi* agiDX9RemixApiScene()
{
    if (!s_active || !s_scene.CreateMaterial || !s_scene.DestroyMaterial || !s_scene.CreateMesh ||
        !s_scene.DestroyMesh || !s_scene.DrawInstance)
        return nullptr;

    return &s_scene;
}

bool agiDX9RemixApiSetConfig(const char* key, const char* value)
{
    if (!s_active || !s_remix.SetConfigVariable || !key || !value)
        return false;

    return s_remix.SetConfigVariable(key, value) == REMIXAPI_ERROR_CODE_SUCCESS;
}

bool agiDX9RemixApiHasGameValues()
{
    return s_active && s_remix.SetGameValue;
}

bool agiDX9RemixApiSetGameValue(const char* key, const char* value)
{
    if (!agiDX9RemixApiHasGameValues() || !key || !value)
        return false;

    return s_remix.SetGameValue(key, value) == REMIXAPI_ERROR_CODE_SUCCESS;
}

void agiDX9RemixApiSubmitFrame()
{
    if (!s_active)
        return;

    ++s_frame;
    ++s_stats.Frames;

    // Never 0: the frame stamp 0 means "never seen" on a fresh slot.
    if (s_frame == 0)
        s_frame = 1;

    const f32 radius = std::max(PARAM_remix_lightradius.get_or(0.15f), 0.01f);

    // 1. Resolve the registry into candidate lights.
    u32 count = 0;

    for (u32 i = 0; i < agiGlowLightCount; ++i)
    {
        if (ResolveGlow(agiGlowLights[i], radius, s_candidates[count]))
            ++count;
    }

    // 2. Budget. The brightest survive; what is cut is typically distant and dim. A light cut this
    // frame is destroyed like one that expired, so a scene hovering at the cap can churn a few
    // lights at the margin - the cap exists to bound the bridge traffic, not to be run against.
    const u32 budget = static_cast<u32>(std::clamp(PARAM_remix_maxlights.get_or(192), 0, static_cast<i32>(kMaxLights)));

    if (count > budget)
    {
        std::nth_element(s_candidates, s_candidates + budget, s_candidates + count,
            [](const Candidate& a, const Candidate& b) { return a.Power > b.Power; });

        s_stats.Culled += count - budget;
        count = budget;
    }

    // 3. Match candidates to the lights already in the runtime, and stamp the ones still wanted.
    RebuildIndex();

    for (u32 i = 0; i < count; ++i)
    {
        Candidate& candidate = s_candidates[i];
        candidate.Slot = FindSlot(candidate.GlowId);

        if (candidate.Slot >= 0)
            s_lights[candidate.Slot].SeenFrame = s_frame;
    }

    // 4. Retire the lights nobody asked for this frame, before creating new ones, so the slots they
    // free are available below.
    for (RemixLight& light : s_lights)
    {
        if ((light.GlowId != 0) && (light.SeenFrame != s_frame))
            DestroySlot(light);
    }

    // 5. Create what is new, re-send what has changed.
    u32 free_cursor = 0;

    for (u32 i = 0; i < count; ++i)
    {
        const Candidate& candidate = s_candidates[i];

        if (candidate.Slot >= 0)
        {
            RemixLight& light = s_lights[candidate.Slot];

            const f32 old_power = light.Radiance.x + light.Radiance.y + light.Radiance.z;
            const Vector3 delta = candidate.Radiance - light.Radiance;
            const f32 radiance_change = std::fabs(delta.x) + std::fabs(delta.y) + std::fabs(delta.z);

            const bool moved = (candidate.Position - light.Position).Mag2() > kMoveEpsilonSq;
            const bool recoloured = radiance_change > (old_power * kRadianceEpsilon);
            const bool resized = light.Radius != candidate.Radius;

            const Shaping& old_shape = light.Shape;
            const Shaping& new_shape = candidate.Shape;
            const bool reshaped = (old_shape.Aimed != new_shape.Aimed) ||
                (new_shape.Aimed &&
                    (((old_shape.Direction ^ new_shape.Direction) < kAimEpsilonCos) ||
                        (old_shape.Cone != new_shape.Cone) || (old_shape.Softness != new_shape.Softness)));

            if (!moved && !recoloured && !resized && !reshaped)
                continue;

            if (!UpdateLight(light, candidate))
            {
                DestroyHandles(light);
                light = {};
                --s_live;
                continue;
            }

            light.Position = candidate.Position;
            light.Radiance = candidate.Radiance;
            light.Radius = candidate.Radius;
            light.Shape = candidate.Shape;
            ++s_stats.Updated;

            continue;
        }

        while ((free_cursor < kMaxLights) && (s_lights[free_cursor].GlowId != 0))
            ++free_cursor;

        // Cannot happen - count <= kMaxLights and step 4 freed every slot not claimed above - but a
        // silent overrun here would be a corrupted light table, so check rather than assume.
        if (free_cursor >= kMaxLights)
            break;

        RemixLight& light = s_lights[free_cursor];

        if (!CreateLight(candidate, 0, light.Handle))
            continue;

        light.GlowId = candidate.GlowId;
        light.Generation = 0;
        light.Position = candidate.Position;
        light.Radiance = candidate.Radiance;
        light.Radius = candidate.Radius;
        light.Shape = candidate.Shape;
        light.SeenFrame = s_frame;

        ++s_live;
        ++s_stats.Created;
    }

    // 6. Draw. The runtime empties its list of drawn API lights every frame, so every live light is
    // drawn every frame, changed or not.
    for (const RemixLight& light : s_lights)
    {
        if ((light.GlowId != 0) && light.Handle)
        {
            s_remix.DrawLightInstance(light.Handle);
            ++s_stats.Drawn;
        }
    }
}

void agiDX9RemixApiReleaseAll()
{
    if (!s_active)
        return;

    for (RemixLight& light : s_lights)
    {
        if (light.GlowId != 0)
            DestroySlot(light);
    }

    s_live = 0;
}

void agiDX9RemixApiLogStats(u32 frame)
{
    if (!s_active)
        return;

    const f32 frames = static_cast<f32>(std::max<u32>(s_stats.Frames, 1));

    Displayf("DX9 REMIXAPI: frame=%u live=%u drawn/frame=%.1f | over %u frames: created=%u updated=%u destroyed=%u "
             "over-budget=%u failed=%u rehashed=%u",
        frame, s_live, static_cast<f32>(s_stats.Drawn) / frames, s_stats.Frames, s_stats.Created, s_stats.Updated,
        s_stats.Destroyed, s_stats.Culled, s_stats.Failed, s_stats.Rehashed);

    s_stats = {};
}
