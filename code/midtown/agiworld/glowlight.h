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

#pragma once

// Not part of the original engine/binary.
//
// The city is full of things that *look* like light sources - street lamps, traffic lights, lit
// signage, coronas, headlight glows - but emit no actual light. They are billboards: an AlphaGlow
// texture composited additively, drawn by agiMeshSet::DrawCard(). The original engine has no
// concept of them illuminating anything.
//
// DrawCard() is handed exactly what a real light needs, though - a WORLD-space position, a colour
// and a size - so the draw stream itself is a usable light-source database, with no new asset
// authoring and no per-city hand placement. This registry harvests those billboards so the
// programmable path (Pathway B, agidx9/dx9shader.cpp) can turn them into real point lights.
//
// Harvesting is deliberately in agiworld rather than in the renderer: DrawCard() is where the
// world-space information exists. By the time a glow reaches the rasterizer it has already been
// projected to screen space and the position is gone.

#include "vector7/vector3.h"

class agiTexDef;

struct agiGlowLight
{
    Vector3 Position;

    // Modulation applied on top of the texture colour resolved below - the mesh/card's own vertex
    // colour and alpha. Not the final colour on its own.
    Vector3 Tint;

    f32 Radius;

    // The glow texture and the point in it this flare actually samples. Resolved to a colour by the
    // renderer, because the pixel data lives there (agidx9/dx9texdef.cpp).
    //
    // UV matters, not just the texture: a traffic light is ONE texture containing red, amber and
    // green, and agiMeshSet::DrawCard picks between them with its `frame` argument, which selects a
    // UV sub-rect out of agiMeshCardInfo::Frames. Averaging the whole texture would give every
    // traffic light the same muddy colour regardless of state; sampling where the flare actually
    // reads is what makes it cycle red/amber/green.
    agiTexDef* Texture;
    f32 U;
    f32 V;

    // Per-kind intensity multiplier, resolved provisionally at harvest time and refined by the
    // renderer once it has sampled the flare's real hue - see agiClassifyGlowIntensity.
    f32 Intensity;

    // World-space movement over the last frame, used to predict where this light will be next
    // frame so a fast-moving one can still be recognised. See agiAddGlowLightRGB.
    Vector3 Velocity;

    // Frames since this slot was last refreshed by a sprite. Drives the expiry fade; 0 means seen
    // this frame. A slot with Texture == nullptr is free.
    u32 Age;

    // Identity of the light this slot holds, stable for as long as the same sprite keeps refreshing
    // it, and never reused within a process. The slot's INDEX is not an identity:
    // agiUpdateGlowLights compacts the array every frame, so a light's index shifts whenever one
    // before it expires. A consumer that keeps its own per-light state across frames - the Remix
    // API, which holds a runtime light per glow - keys it on this. 0 only in a free slot.
    u32 Id;

    // World-space unit vector the light is aimed along, or zero for a light that shines every way.
    // Only headlights have one: the harvest recovers it from the beam mesh the game draws (see
    // HarvestHeadlightBeam in agidx9/dx9rsys.cpp). ConeAngle is the beam's half-angle in degrees,
    // measured off the same mesh, and means nothing when Direction is zero.
    Vector3 Direction;
    f32 ConeAngle;

    // Where the flare sits in its owner's own space - the car or the lamp model - before the owner's
    // world transform, when the harvest knows it (HasLocal). It is what identifies a lamp from frame
    // to frame: a car's tail light is at the same model-space point in every frame however fast the
    // car moves, while its world position jumps a metre or more per frame at speed. See
    // agiAddGlowLightRGB.
    Vector3 Local;
    u32 HasLocal;
};

// Whether the glow harvest runs at all. Off until a consumer needs it: the harvest, the registry's
// per-frame ageing and each glow texture's colour grid are all pure cost with nobody reading them.
// Set by the Remix API once it connects (agidx9/dx9remix.cpp).
extern bool agiGlowHarvestEnabled;

// How long a light survives without its sprite being redrawn, and how much of that it spends
// fading. Long enough to ride out a culling or LOD hiccup, short enough that a light genuinely
// switched off - a traffic light changing state - does not visibly linger.
inline constexpr u32 AGI_GLOW_LIGHT_TTL = 12;
inline constexpr u32 AGI_GLOW_LIGHT_FADE = 6;

// Bounding radius of the mesh currently being submitted through the native-transform path, in world
// units. Set by agiMeshSet::DrawNativeTransform.
//
// No longer read by the light path. It existed because light selection used to be PER DRAW, and
// testing a light against the instance ORIGIN alone is wrong for anything large - a road section or
// a building shell is tens of units across, and its origin can sit far outside the reach of a lamp
// that is nonetheless shining on one end of it. (That mistake is why lights initially appeared to
// affect only the player's car: a car's origin sits right on its own lights, so it was the one mesh
// that always passed.) Clustering removed the question entirely - the pixel shader looks lights up
// by its own world position, so there is no per-mesh approximation left to get wrong.
//
// Kept because it is a genuinely useful thing to know about the draw in flight, and the frustum
// culling described in §1.3 of the design doc will want it.
extern f32 agiNativeDrawRadius;

// Reflectivity of the draw in flight: 0 for ordinary geometry, 1 for a vehicle body. Set by
// agiMeshSet::DrawLitSph and consumed by the DX9 world shader to pick up the environment probe.
// See the note where it is defined (agiworld/meshrend.cpp).
extern f32 agiNativeReflectivity;

// The vehicle's own authored sphere map for the draw in flight, or null. See its definition.
class agiTexDef;
extern agiTexDef* agiNativeReflectionTex;

// Lightning intensity, 0..1, latched and decayed once per frame by mmCullCity::Cull().
//
// The original lightning is a one-frame swap of the sky texture (see mmSky::DoFlash) and lights
// nothing. A real strike floods the whole scene for a moment, so this is held and decayed over
// several frames and fed to the world shader as a broad burst of sky irradiance. It has to be
// latched rather than read directly, because mmSky::Draw clears the flag as it draws - and the sky
// is drawn before the city, so by the time any city geometry renders the flag is already gone.
extern f32 agiLightningFlash;

// Sized to comfortably cover the glows visible in one frame of dense city. Overflow is dropped
// rather than grown: this runs inside the draw loop, and the consumer only keeps the nearest few
// per draw anyway, so a hard cap costs nothing visible and keeps the cost bounded.
inline constexpr u32 AGI_MAX_GLOW_LIGHTS = 512;

// The live light set. PERSISTENT across frames, not rebuilt each frame.
//
// This used to be double-buffered - harvest into one list, read last frame's - which tied a light's
// existence to whether its sprite happened to be drawn that frame. That is the wrong lifetime, and
// it is the cause of the reported flickering: a lamp whose billboard is momentarily culled
// (off-screen, an LOD switch, a cell boundary) had its light vanish outright and snap back a frame
// later. Nothing about the lamp changed - only whether the renderer happened to draw its sprite.
//
// Lights now live in slots refreshed when their sprite is seen, expiring on a timer when it is not
// and fading over the last few frames rather than popping. A slot is also updated in place the
// moment it is re-harvested, so it tracks its owner within the same frame instead of trailing by
// one - which is what made vehicle lights lag behind moving cars.
extern agiGlowLight agiGlowLights[AGI_MAX_GLOW_LIGHTS];
extern u32 agiGlowLightCount;

// Harvest accounting for one frame, reset by agiUpdateGlowLights and reported by the census.
//
// These exist to answer one question that no amount of looking at the screen can: when a street lamp
// emits no light, is its billboard *not reaching* the harvest hook, or is it reaching it and being
// rejected? Those are opposite bugs. DrawCard reads the glow texture from agiCurState, which is
// global state a closed caller is expected to have populated - if a banger's DrawGlow() binds its
// texture some other way, the read comes back null and the lamp is dropped before anything else in
// the pipeline ever sees it.
//
//   CardsSeen        - DrawCard calls that survived the depth reject
//   CardsNoTexture   - ...of which had no agiCurState texture at all
//   CardsNotGlow     - ...of which had one, but not flagged agiTexProp::AlphaGlow
//   CardsHarvested   - ...of which were registered as lights
extern u32 agiGlowCardsSeen;
extern u32 agiGlowCardsNoTexture;
extern u32 agiGlowCardsNotGlow;
extern u32 agiGlowCardsHarvested;

// How far a flare of the given half-extent is taken to throw, in world units.
//
// SHARED between both harvest routes on purpose. They used to disagree - the billboard route floored
// the reach at 24 units and the mesh route at 14 - and because emitted intensity scales with the
// SQUARE of reach (see dx9shader.cpp), the same physical fixture came out roughly 3x brighter
// depending on whether the engine happened to draw it as a card or as glow geometry.
//
// The engine records nothing about how far a lamp throws; all it has is how big the flare is drawn.
// Both the multiplier and the floor are therefore judgement calls, which is why both are cmd_params:
// the floor matters more, because without it a small distant flare produces a light with a metre of
// reach that cannot illuminate anything but itself.
f32 agiGlowLightReach(f32 flare_half_extent);

// Per-kind intensity multiplier for a glow, from its texture name and its emitted colour.
//
// Exposed rather than kept private to the harvester because the harvester only knows the flare's
// TINT (the card/mesh vertex colour), while the actual emitted hue is that tint modulated by the
// glow texture itself - and only the renderer can sample the texture (agiDX9TexDef::SampleGlowColor).
// The renderer re-runs this with the resolved colour so classification and colour agree.
//
// `color` may be at any brightness: the test is on RELATIVE saturation, so a flare that has faded to
// a tenth of its brightness still classifies as the same kind of light.
// What sort of light a glow is. These are the distinctions the draw stream actually supports:
// the engine records no "kind" field, so everything below is recovered from the glow texture's name
// and the flare's tint. Brake and tail are one kind on purpose - both draw FXLTGLOWRED off the same
// mesh with no state flag between them, so splitting them would be a guess presented as a setting.
enum class agiGlowKind
{
    Headlight, // FXLTCONE - the headlight cone mesh
    Vehicle,   // FXLTGLOWRED / FXLTGLOWAMBER - tail and brake lamps
    Traffic,   // a pure hue - traffic signals
    Lamp,      // warm near-white - street lamps
    Generic,   // neutral white - reverse lamps, coronas, everything else
};

agiGlowKind agiClassifyGlowKind(const char* name, const Vector3& color);

// False when the ini or command line has switched this kind off. Checked at harvest time, so a
// disabled kind costs no pool slot and no cell-grid entry.
bool agiGlowKindEnabled(agiGlowKind kind);

f32 agiClassifyGlowIntensity(const char* name, const Vector3& color);

// Called by agiMeshSet::DrawCard for each AlphaGlow billboard it submits.
//
// `local` is the flare's position in its owner's own space, before the world transform - see
// agiGlowLight::Local. Null when not known.
void agiAddGlowLight(
    const Vector3& position, u32 color, f32 scale, agiTexDef* texture, f32 u, f32 v, const Vector3* local = nullptr);

// Same, for glows submitted as world-space GEOMETRY rather than as billboards.
//
// Not every glow in the game is a card. Vehicle lighting in particular is not: both
// mmCarModel::DrawGlow (the player) and aiVehicleInstance::DrawGlow (traffic) select a glow mesh
// out of mmInstance::MeshSetTable and submit it through agiMeshSet::Draw, so headlights,
// tail lights, brake lights and reverse lights never reach DrawCard at all. Bangers - street lamps,
// traffic lights, lit signage - do use DrawCard. Harvesting both is what makes the coverage
// complete.
//
// Colour is passed already resolved rather than packed, because the geometry path has two sources
// to combine: the mesh's per-vertex colour and agiTexParameters::Color, the texture's own
// representative colour (agiworld/getmesh.cpp: `tex.Color = prop->Color`, with a day/night variant
// in the sheet data). That is the engine's own answer to "what colour is this light", which is why
// a red tail light, an amber indicator and a green traffic light come out right without anything
// being hard-coded here.
//
// `direction` and `cone_angle` make it an aimed light - see agiGlowLight::Direction. Zero, the
// default, is a light that shines every way, which is every glow but a headlight.
void agiAddGlowLightRGB(const Vector3& position, const Vector3& tint, f32 radius, agiTexDef* texture, f32 u, f32 v,
    const Vector3& direction = Vector3 {0.0f, 0.0f, 0.0f}, f32 cone_angle = 0.0f, const Vector3* local = nullptr);

// Ages the live set and retires lights whose sprite has not been seen for a while.
// Called once per frame by the pipeline (agidx9/dx9pipe.cpp, BeginFrame).
void agiUpdateGlowLights();

// Drops every harvested light. Must be called when the pipeline is torn down.
//
// agiGlowLight::Texture is an agiTexDef* borrowed from the draw the light was harvested out of, but
// this registry is a process-lifetime global while the texture is owned by the pipeline. Quitting a
// race back to the menu destroys the pipeline AND resets the engine's memory arena, freeing every
// agiTexDef wholesale, so without this the registry survives holding dangling pointers - and the
// very next BeginFrame walks them (agiDX9WorldShader::UpdateLights -> BuildLightPool ->
// agiDX9TexDef::SampleGlowColor) and faults. The lights described here belong to a city that no
// longer exists, so there is nothing to preserve.
void agiResetGlowLights();

// 0..1 fade for a light, from its age. Reaches 0 exactly as the light expires.
inline f32 agiGlowLightFade(u32 age)
{
    if (age <= (AGI_GLOW_LIGHT_TTL - AGI_GLOW_LIGHT_FADE))
        return 1.0f;

    if (age >= AGI_GLOW_LIGHT_TTL)
        return 0.0f;

    return static_cast<f32>(AGI_GLOW_LIGHT_TTL - age) / static_cast<f32>(AGI_GLOW_LIGHT_FADE);
}
