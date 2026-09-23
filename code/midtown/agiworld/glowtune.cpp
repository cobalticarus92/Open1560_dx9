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

define_dummy_symbol(agiworld_glowtune);

#include "glowtune.h"

#include <cctype>
#include <cstring>

// Fixed storage, filled once at startup. Deliberately not a std container: it is read from the draw
// path for the life of the process, and the engine resets its memory arena between races - anything
// allocated through the engine's heap would be freed underneath it (see agiResetGlowLights).
static constexpr u32 kMaxTextureTunings = 64;
static constexpr u32 kKindCount = 5;

struct TextureTuning
{
    char Name[32];
    agiGlowTuning Tuning;
};

static agiGlowTuning s_kind_tuning[kKindCount] {};
static TextureTuning s_texture_tuning[kMaxTextureTunings] {};
static u32 s_texture_tuning_count = 0;

static bool EqualNoCase(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
    {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }

    return *a == *b;
}

agiGlowTuning& agiGlowKindTuning(agiGlowKind kind)
{
    const u32 index = static_cast<u32>(kind);
    return s_kind_tuning[(index < kKindCount) ? index : (kKindCount - 1)];
}

static const agiGlowTuning* FindTextureTuning(const char* texture_name)
{
    if (!texture_name)
        return nullptr;

    for (u32 i = 0; i < s_texture_tuning_count; ++i)
    {
        if (EqualNoCase(s_texture_tuning[i].Name, texture_name))
            return &s_texture_tuning[i].Tuning;
    }

    return nullptr;
}

agiGlowTuning* agiGlowTextureTuning(const char* texture_name)
{
    if (!texture_name || !*texture_name)
        return nullptr;

    if (const agiGlowTuning* existing = FindTextureTuning(texture_name))
        return const_cast<agiGlowTuning*>(existing);

    if (s_texture_tuning_count >= kMaxTextureTunings)
        return nullptr;

    TextureTuning& entry = s_texture_tuning[s_texture_tuning_count++];
    arts_strncpy(entry.Name, texture_name, ARTS_TRUNCATE);
    entry.Tuning = {};

    return &entry.Tuning;
}

u32 agiGlowTextureTuningCount()
{
    return s_texture_tuning_count;
}

agiGlowTuning agiResolveGlowTuning(const char* texture_name, agiGlowKind kind)
{
    agiGlowTuning result = agiGlowKindTuning(kind);

    const agiGlowTuning* texture = FindTextureTuning(texture_name);

    if (!texture)
        return result;

    // Field by field, so a texture section only has to say what differs from its kind.
    if (texture->HasEnabled)
    {
        result.HasEnabled = true;
        result.Enabled = texture->Enabled;
    }

    if (texture->HasOffset)
    {
        result.HasOffset = true;
        result.Offset = texture->Offset;
    }

    if (texture->HasOutward)
    {
        result.HasOutward = true;
        result.Outward = texture->Outward;
    }

    if (texture->HasIntensity)
    {
        result.HasIntensity = true;
        result.Intensity = texture->Intensity;
    }

    if (texture->HasRadius)
    {
        result.HasRadius = true;
        result.Radius = texture->Radius;
    }

    if (texture->HasColor)
    {
        result.HasColor = true;
        result.Color = texture->Color;
    }

    return result;
}

Vector3 agiGlowLocalOffset(const agiGlowTuning& tuning, const Vector3& local_position)
{
    if (!tuning.HasOffset)
        return {0.0f, 0.0f, 0.0f};

    Vector3 offset = tuning.Offset;

    // A lamp ON the centre line (a centre brake light) has no outward, so it is left where it is
    // in X rather than being pushed arbitrarily to one side.
    if (tuning.HasOutward && tuning.Outward)
    {
        if (local_position.x > 0.0f)
            offset.x = tuning.Offset.x;
        else if (local_position.x < 0.0f)
            offset.x = -tuning.Offset.x;
        else
            offset.x = 0.0f;
    }

    return offset;
}

// Indexed by agiGlowKind, so its declaration order is load-bearing here.
static_assert(static_cast<u32>(agiGlowKind::Headlight) == 0, "agiGlowKind order changed");
static_assert(static_cast<u32>(agiGlowKind::Vehicle) == 1, "agiGlowKind order changed");
static_assert(static_cast<u32>(agiGlowKind::Traffic) == 2, "agiGlowKind order changed");
static_assert(static_cast<u32>(agiGlowKind::Lamp) == 3, "agiGlowKind order changed");
static_assert(static_cast<u32>(agiGlowKind::Generic) == 4, "agiGlowKind order changed");

static const char* const kKindNames[kKindCount] {
    "Headlights",     // agiGlowKind::Headlight
    "VehicleLamps",   // agiGlowKind::Vehicle
    "TrafficSignals", // agiGlowKind::Traffic
    "StreetLamps",    // agiGlowKind::Lamp
    "OtherGlows",     // agiGlowKind::Generic
};

const char* agiGlowKindName(agiGlowKind kind)
{
    const u32 index = static_cast<u32>(kind);
    return (index < kKindCount) ? kKindNames[index] : "OtherGlows";
}

bool agiGlowKindFromName(const char* name, agiGlowKind& out_kind)
{
    for (u32 i = 0; i < kKindCount; ++i)
    {
        if (EqualNoCase(kKindNames[i], name))
        {
            out_kind = static_cast<agiGlowKind>(i);
            return true;
        }
    }

    return false;
}
