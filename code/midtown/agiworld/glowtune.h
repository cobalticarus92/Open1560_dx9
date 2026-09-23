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
// Hand tuning for glow lights, per kind and per glow texture - filled from Open1560_RemixAPI.ini
// (agidx9/dx9config.cpp) and applied where each value is known: offsets at harvest, where the
// flare's own transform is still at hand; brightness, colour and size where the light is resolved
// for the Remix API (agidx9/dx9remix.cpp).
//
// Why offsets at all. The light is placed at the centre of the flare the game draws, and the flare
// is drawn ON the lamp: a tail light's glow card sits on the lens, a street lamp's corona round the
// bulb. For a rasterised glow that is exactly right. For a path-traced light it often is not - a
// sphere light at the lens centre is half inside the car body, which shadows it, and a street lamp
// lit from its corona centre can sit inside the lamp housing. A small, per-lamp nudge out of the
// geometry is what makes it light the street. There is no general rule for how far - it depends on
// how each model was built - which is why it is a setting per kind and per texture, not a constant.

#include "glowlight.h"

struct agiGlowTuning
{
    // Each field applies only when its Has* flag is set, so a per-texture section can override one
    // value and inherit the rest from its kind.
    bool HasEnabled {};
    bool Enabled {true};

    // In the flare's LOCAL space, before its world transform: X across, Y up, Z along the object.
    // Vehicles drive toward -Z (aiVehicleMGR: velocity = m2 * -speed), so +Z is the rear. An offset
    // stays attached to the lamp however the car turns. Engine units (about a metre).
    bool HasOffset {};
    Vector3 Offset {};

    // Apply the X offset away from the model's centre line rather than always toward +X, so one
    // setting moves both of a pair of lamps outward (or, negative, inward) together.
    bool HasOutward {};
    bool Outward {};

    // Brightness multiplier in the same units as the per-kind -light* switches (lightlamp, ...).
    // In a per-kind section this is those switches; in a per-texture section it replaces the kind's.
    bool HasIntensity {};
    f32 Intensity {1.0f};

    // Radius of the Remix sphere light, world units. Unset means -remixlightradius.
    bool HasRadius {};
    f32 Radius {};

    // Multiplies the resolved colour, per channel. For correcting a hue the glow sheet gets wrong.
    bool HasColor {};
    Vector3 Color {1.0f, 1.0f, 1.0f};
};

// Tuning for a glow kind. Written only by the config loader, before the game starts.
agiGlowTuning& agiGlowKindTuning(agiGlowKind kind);

// Tuning for one glow texture (by agiTexDef::Tex.Name, case-insensitive), created on first use.
// Null when the table is full. Written only by the config loader.
agiGlowTuning* agiGlowTextureTuning(const char* texture_name);

// The effective tuning for a flare: its kind's section, with the texture's own section over it field
// by field. `texture_name` may be null.
agiGlowTuning agiResolveGlowTuning(const char* texture_name, agiGlowKind kind);

// The local-space offset for a flare whose local position is `local_position`, or zero. `outward`
// resolves against local_position.x.
Vector3 agiGlowLocalOffset(const agiGlowTuning& tuning, const Vector3& local_position);

// Kind name as used in the config file ("StreetLamps", ...), and back. FromName is
// case-insensitive and returns false for an unknown name.
const char* agiGlowKindName(agiGlowKind kind);
bool agiGlowKindFromName(const char* name, agiGlowKind& out_kind);

// Number of per-texture sections loaded, for the startup log.
u32 agiGlowTextureTuningCount();
