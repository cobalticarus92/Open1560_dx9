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

#include "dx9config.h"

#include "agiworld/glowtune.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// std::fopen is deprecated by MSVC and this build is /WX. The alternatives are fopen_s (Annex K) or
// routing through the engine's Stream layer - but that layer is sandboxed by HierAllowPath, which
// is not even configured yet at the point this runs, and cannot write files. Reading and writing one
// small config file in the game directory at startup is not the unsafety the deprecation is aimed
// at, so suppress it locally. Same treatment as std::getenv in agiworld/meshrend.cpp.
#pragma warning(push)
#pragma warning(disable : 4996)

define_dummy_symbol(agidx9_dx9config);

static constexpr const char* kConfigName = "Open1560_RemixAPI.ini";

// The file this one replaced. Read once, to carry its settings over, and never again - see
// agiDX9LoadRemixConfig.
static constexpr const char* kLegacyConfigName = "Open1560-Shaders.ini";

// Written verbatim when the file does not exist. Every value shown is the built-in default, so the
// generated file is a no-op until something is edited.
static constexpr const char* kConfigTemplate =
    "; Open1560 - RTX Remix API configuration\n"
    ";\n"
    "; What the game sends to RTX Remix through the Remix API: its glow sprites as real lights, and\n"
    "; (Remix Plus) the sky and weather. See docs/remix_api_plan.md for how each part works.\n"
    ";\n"
    "; Keys outside the [Glow...] sections are command line switches of the same name, and the command\n"
    "; line WINS over this file, so a setting can be overridden for one run without editing anything.\n"
    "; Values shown are the defaults. Delete this file to regenerate it.\n"
    "\n"
    "[RemixAPI]\n"
    "; Connect to the Remix API and send the game's glow sprites as real lights. Needs the Remix bridge\n"
    "; with `exposeRemixApi = True` in its bridge.conf (NVIDIA's or Remix Plus's).\n"
    "remixapi = 0\n"
    "; Overall brightness of every glow light, before the per-kind intensity below.\n"
    "remixlightpower = 1.5\n"
    "; Size of each light's emitter, in world units. Changes how soft its shadows are, not how bright\n"
    "; it is. A [Glow...] section can set its own.\n"
    "remixlightradius = 0.15\n"
    "; Most lights sent per frame. The brightest are kept.\n"
    "remixmaxlights = 192\n"
    "; Remix options (rtx.conf keys) set through the API once it connects: key=value|key=value\n"
    "; remixconfig = rtx.fallbackLightMode=0\n"
    "; Log the first 64 lights as they are created.\n"
    "remixapidebug = 0\n"
    "\n"
    "[GlowReach]\n"
    "; Flare size to how far a light throws, in world units, and the shortest reach any light may\n"
    "; have. The engine records only how big a flare is drawn, not how far its lamp throws. Brightness\n"
    "; goes with the SQUARE of reach, so raising either brightens lights as well as widening them.\n"
    "glowreachscale = 14.0\n"
    "glowreachmin = 20.0\n"
    "\n"
    "; ---------------------------------------------------------------------------------------------\n"
    "; GLOW LIGHTS, PER KIND\n"
    ";\n"
    "; Every glow sprite is sorted into one of five kinds, by its texture name and colour. Each kind\n"
    "; has a section with these keys:\n"
    ";\n"
    ";   enabled    1 or 0. 0 removes that kind's light (the flare itself still draws).\n"
    ";   intensity  brightness of this kind, on top of remixlightpower.\n"
    ";   offset     X Y Z, moves the light from the centre of its flare, in the OBJECT'S OWN SPACE\n"
    ";              before it is placed in the world: X across, Y up, Z along its length. Vehicles\n"
    ";              drive toward -Z, so +Z is toward the REAR. For a car the offset stays on the lamp\n"
    ";              however the car turns. Engine units, about a metre.\n"
    ";   outward    1 makes the X offset point away from the object's centre line, so one setting\n"
    ";              moves both lamps of a pair out (or, negative, in) together. 0: always toward +X.\n"
    ";   radius     this kind's emitter size, overriding remixlightradius.\n"
    ";   color      R G B multiplier on the light's colour, e.g. 1 0.9 0.8 to warm it.\n"
    ";   cone       headlights only: the beam's half-angle in degrees. Unset: measured off the beam.\n"
    ";   softness   headlights only: how gradual the beam's edge is, 0 (hard) to 1.\n"
    ";\n"
    "; Why offsets. The light starts at the centre of the flare, and the flare is drawn ON the lamp:\n"
    "; a tail light's glow sits on the lens, a street lamp's round the bulb. A path-traced light there\n"
    "; can end up inside the car body or the lamp housing, which then shadows it. A few centimetres\n"
    "; out of the geometry is usually all it takes to make it light the street.\n"
    "; ---------------------------------------------------------------------------------------------\n"
    "\n"
    "[Glow.StreetLamps]\n"
    "; Warm, unsaturated glows: street lamps and other static lighting.\n"
    "enabled = 1\n"
    "intensity = 10.0\n"
    "offset = 0 0 0\n"
    "outward = 0\n"
    "\n"
    "[Glow.TrafficSignals]\n"
    "; Pure-hue glows. Each signal takes the colour it is showing.\n"
    "enabled = 1\n"
    "intensity = 2.0\n"
    "offset = 0 0 0\n"
    "outward = 0\n"
    "\n"
    "[Glow.VehicleLamps]\n"
    "; Tail and brake lamps (FXLTGLOWRED, FXLTGLOWAMBER). One kind: the game draws both from the same\n"
    "; sheet with nothing to tell them apart. +Z is toward the rear, so a small positive Z pushes a tail\n"
    "; light back out of the body.\n"
    "enabled = 1\n"
    "intensity = 1.25\n"
    "offset = 0 0 0\n"
    "outward = 1\n"
    "\n"
    "[Glow.Headlights]\n"
    "; Headlights, as spot lights aimed down the beam. The game draws each beam as a cone mesh\n"
    "; (FXLTCONE); the light is put at the lamp end of it and aimed along it, one per lamp, and the\n"
    "; beam's length is its reach. The white flares on the front of a vehicle count as headlights too,\n"
    "; so enabled = 0 turns off every light at the headlamps (glowfrontheadlights = 0 sends those\n"
    "; flares back to StreetLamps/OtherGlows). The flares and beams themselves still draw. offset moves the lamp end "
    "(-Z is forward, so a small negative Z\n"
    "; brings the light out of the headlight housing). cone is the beam's half-angle in degrees,\n"
    "; measured off the mesh when unset; softness is how gradual its edge is, 0 to 1.\n"
    "enabled = 1\n"
    "intensity = 2.0\n"
    "offset = 0 0 0\n"
    "outward = 1\n"
    "; cone = 25\n"
    "softness = 0.3\n"
    "\n"
    "[Glow.OtherGlows]\n"
    "; Neutral-white glows that are none of the above: reverse lamps, coronas.\n"
    "enabled = 1\n"
    "intensity = 1.0\n"
    "offset = 0 0 0\n"
    "outward = 0\n"
    "\n"
    "; ---------------------------------------------------------------------------------------------\n"
    "; GLOW LIGHTS, PER TEXTURE\n"
    ";\n"
    "; A [Glow:<TEXTURE>] section targets every flare drawn with one glow texture, exactly, whatever\n"
    "; kind it sorts into. It takes the same keys as a kind section, and only the keys it sets\n"
    "; override its kind; the rest are inherited. Here `intensity` REPLACES the kind's brightness.\n"
    "; Texture names are not case sensitive. To find them, set glowdebug = 1 in [Debug] and read the\n"
    "; log: every glow texture is listed as it is first seen, as GLOW: tex='NAME'. Up to 64 sections.\n"
    ";\n"
    "; Examples (remove the ; to use them):\n"
    ";\n"
    "; [Glow:FXLTGLOWRED]\n"
    "; offset = 0.05 0 0.08\n"
    "; outward = 1\n"
    ";\n"
    "; [Glow:FXLTGLOW]\n"
    "; offset = 0 -0.25 0\n"
    "; radius = 0.2\n"
    "; ---------------------------------------------------------------------------------------------\n"
    "\n"
    "[RemixSky]\n"
    "; RTX Remix Plus only: drive its physical sky from the race's time of day and weather - sun,\n"
    "; moon and stars, volumetric clouds, and a weather preset per game weather. Hides the game's own\n"
    "; sky dome (through its Textured Sky option) while it runs.\n"
    "remixsky = 1\n"
    "; Remix Plus weather preset used for each game weather.\n"
    "remixskyclear = clear\n"
    "remixskyfog = foggy\n"
    "remixskyrain = rainstorm\n"
    "remixskysnow = snow\n"
    "; Set each preset's fog density from the game's own fog distances, and scale it.\n"
    "remixskyfogmatch = 1\n"
    "remixskyfogdensity = 1.0\n"
    "; Lightning on the game's thunder rather than at random.\n"
    "remixlightningsync = 1\n"
    "; Remix Plus's own rain and snow. The game draws its own, so tag its particle textures Ignore\n"
    "; in Remix before turning this on, or the rain falls twice.\n"
    "remixprecipitation = 0\n"
    "; Degrees added to the sun and moon azimuth, to turn the sky to the city.\n"
    "remixsunrotation = 0.0\n"
    "\n"
    "[RemixWet]\n"
    "; RTX Remix wet roads: a puddle-and-damp layer laid over the road while it rains, through the\n"
    "; Remix API. The game generates the puddle pattern from noise (cached in Open1560_RemixWet\\)\n"
    "; and the path tracer reflects the city in it. Needs remixapi = 1.\n"
    "remixwet = 1\n"
    "; Wetness in every weather, 0 to 1, overriding the four below. -1 follows the weather.\n"
    "remixwetlevel = -1\n"
    "; How wet the road is in each weather, 0 to 1.\n"
    "remixwetrain = 1.0\n"
    "remixwetsnow = 0.35\n"
    "remixwetfog = 0.0\n"
    "remixwetclear = 0.0\n"
    "; Share of the road under standing water at full wetness, and the size of the repeating pattern\n"
    "; in world units. Change the seed for a different pattern.\n"
    "remixwetcoverage = 0.2\n"
    "remixwettile = 64.0\n"
    "remixwetseed = 1\n"
    "; Roughness of standing water and of damp road.\n"
    "remixwetpuddleroughness = 0.03\n"
    "remixwetdamproughness = 0.28\n"
    "; Everything else gets glossier in the rain too: Remix's default roughness for game textures\n"
    "; (rtx.legacyMaterial.roughnessConstant) at full wetness, and the value to restore when dry.\n"
    "; 0 leaves it alone.\n"
    "remixwetgloss = 0.45\n"
    "remixwetdryroughness = 0.7\n"
    "\n"
    "[Geometry]\n"
    "; Rebuild smooth vertex normals for the hardware path. The engine stores normals as an index into\n"
    "; a 198-entry direction table, coarse enough that a facet's corners often share one normal and\n"
    "; shade flat. This re-averages them in float, with a hard-edge threshold so boxes stay boxes.\n"
    "smoothnormals = 1\n"
    "\n"
    "[Debug]\n"
    "; Log each glow texture as it is first seen, with its name, position, size and tint. This is how\n"
    "; to find the name for a [Glow:<TEXTURE>] section.\n"
    "glowdebug = 0\n";

static char* TrimInPlace(char* text)
{
    while (*text && (static_cast<u8>(*text) <= ' '))
        ++text;

    char* end = text + std::strlen(text);

    while ((end > text) && (static_cast<u8>(end[-1]) <= ' '))
        --end;

    *end = '\0';
    return text;
}

static bool EqualNoCase(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
    {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }

    return *a == *b;
}

static bool StartsWithNoCase(const char* text, const char* prefix)
{
    for (; *prefix; ++text, ++prefix)
    {
        if (std::tolower(static_cast<unsigned char>(*text)) != std::tolower(static_cast<unsigned char>(*prefix)))
            return false;
    }

    return true;
}

static bool ParseBool(const char* text, bool& out)
{
    if (EqualNoCase(text, "1") || EqualNoCase(text, "true") || EqualNoCase(text, "yes") || EqualNoCase(text, "on"))
    {
        out = true;
        return true;
    }

    if (EqualNoCase(text, "0") || EqualNoCase(text, "false") || EqualNoCase(text, "no") || EqualNoCase(text, "off"))
    {
        out = false;
        return true;
    }

    return false;
}

static bool ParseFloat(const char* text, f32& out)
{
    char* end = nullptr;
    const f32 value = std::strtof(text, &end);

    if (end == text)
        return false;

    out = value;
    return true;
}

// "x y z", "x, y, z" or "x,y,z".
static bool ParseVector3(const char* text, Vector3& out)
{
    f32 values[3] {};
    const char* cursor = text;

    for (f32& value : values)
    {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',')
            ++cursor;

        char* end = nullptr;
        value = std::strtof(cursor, &end);

        if (end == cursor)
            return false;

        cursor = end;
    }

    out = {values[0], values[1], values[2]};
    return true;
}

// Where a [Glow...] section's enabled/intensity go for a kind. They are the existing switches, so the
// section and the command line are two ways of setting the same value rather than two values that
// could disagree. Headlights are enabled by -remixheadlights, not -glowheadlights: the latter is on
// by default for the harvest, and it is the Remix side that keeps headlights out unless asked.
struct KindSwitches
{
    const char* Enabled;
    const char* Intensity;
};

static KindSwitches SwitchesForKind(agiGlowKind kind)
{
    switch (kind)
    {
        case agiGlowKind::Headlight: return {"remixheadlights", "lighthead"};
        case agiGlowKind::Vehicle: return {"glowvehiclelights", "lightvehicle"};
        case agiGlowKind::Traffic: return {"glowtrafficlights", "lighttraffic"};
        case agiGlowKind::Lamp: return {"glowstreetlamps", "lightlamp"};
        default: return {"glowgenericlights", "lightgeneric"};
    }
}

struct Section
{
    enum class Type
    {
        Plain,   // key = value, each an existing switch
        Kind,    // [Glow.<Kind>]
        Texture, // [Glow:<TEXTURE>]
        Invalid, // a [Glow...] section that could not be resolved; its keys are skipped
    };

    Type SectionType {Type::Plain};
    agiGlowKind Kind {agiGlowKind::Generic};
    agiGlowTuning* Tuning {};
};

static Section ParseSectionHeader(const char* file_name, char* header, i32 line_number)
{
    // `header` is the text between the brackets.
    char* name = TrimInPlace(header);

    Section section;

    if (StartsWithNoCase(name, "Glow."))
    {
        const char* kind_name = name + 5;

        if (!agiGlowKindFromName(kind_name, section.Kind))
        {
            Warningf("%s:%i: unknown glow kind [%s] - expected StreetLamps, TrafficSignals, VehicleLamps, "
                     "Headlights or OtherGlows",
                file_name, line_number, name);
            section.SectionType = Section::Type::Invalid;
            return section;
        }

        section.SectionType = Section::Type::Kind;
        section.Tuning = &agiGlowKindTuning(section.Kind);
        return section;
    }

    if (StartsWithNoCase(name, "Glow:"))
    {
        const char* texture_name = TrimInPlace(name + 5);

        section.Tuning = agiGlowTextureTuning(texture_name);
        section.SectionType = section.Tuning ? Section::Type::Texture : Section::Type::Invalid;

        if (!section.Tuning)
            Warningf("%s:%i: [%s] skipped - no texture name, or more than 64 texture sections", file_name, line_number,
                name);

        return section;
    }

    return section;
}

// Parses into a temporary and commits only on success, so a mistyped value leaves whatever an earlier
// line set rather than clearing it.
template <typename T>
static bool Assign(bool (*parse)(const char*, T&), const char* text, bool& has, T& out)
{
    T value {};

    if (!parse(text, value))
        return false;

    out = value;
    has = true;
    return true;
}

// A key in a [Glow...] section. Returns false if the key or its value is not understood.
static bool ApplyGlowKey(const Section& section, const char* key, const char* value)
{
    agiGlowTuning& tuning = *section.Tuning;

    // In a kind section, enabled and intensity are the kind's existing switches. In a texture section
    // they are overrides held in the tuning table like everything else.
    if (section.SectionType == Section::Type::Kind)
    {
        const KindSwitches switches = SwitchesForKind(section.Kind);

        if (EqualNoCase(key, "enabled"))
        {
            bool enabled = false;

            if (!ParseBool(value, enabled))
                return false;

            mem::cmd_param::set(switches.Enabled, enabled ? "1" : "0");
            return true;
        }

        if (EqualNoCase(key, "intensity"))
        {
            f32 intensity = 0.0f;

            if (!ParseFloat(value, intensity))
                return false;

            mem::cmd_param::set(switches.Intensity, value);
            return true;
        }
    }
    else
    {
        if (EqualNoCase(key, "enabled"))
            return Assign(ParseBool, value, tuning.HasEnabled, tuning.Enabled);

        if (EqualNoCase(key, "intensity"))
            return Assign(ParseFloat, value, tuning.HasIntensity, tuning.Intensity);
    }

    if (EqualNoCase(key, "offset"))
        return Assign(ParseVector3, value, tuning.HasOffset, tuning.Offset);

    if (EqualNoCase(key, "outward"))
        return Assign(ParseBool, value, tuning.HasOutward, tuning.Outward);

    if (EqualNoCase(key, "radius"))
        return Assign(ParseFloat, value, tuning.HasRadius, tuning.Radius);

    if (EqualNoCase(key, "color") || EqualNoCase(key, "colour"))
        return Assign(ParseVector3, value, tuning.HasColor, tuning.Color);

    if (EqualNoCase(key, "cone"))
        return Assign(ParseFloat, value, tuning.HasCone, tuning.Cone);

    if (EqualNoCase(key, "softness"))
        return Assign(ParseFloat, value, tuning.HasSoftness, tuning.Softness);

    return false;
}

// Reads one file. Returns false if it could not be opened.
static bool LoadConfigFile(const char* file_name, i32& out_applied)
{
    std::FILE* file = std::fopen(file_name, "rb");

    if (!file)
        return false;

    char line[512];
    i32 line_number = 0;
    Section section;

    while (std::fgets(line, sizeof(line), file))
    {
        ++line_number;

        // Strip comments. Both ';' and '#' are accepted, and either may follow a value on the same
        // line, which is what people expect from an ini.
        for (char* c = line; *c; ++c)
        {
            if ((*c == ';') || (*c == '#'))
            {
                *c = '\0';
                break;
            }
        }

        char* text = TrimInPlace(line);

        if (*text == '\0')
            continue;

        // Section headers carry meaning only for [Glow...]. Everything else is a globally named
        // switch, and those sections exist purely to make the file readable.
        if (*text == '[')
        {
            if (char* close = std::strchr(text, ']'))
                *close = '\0';

            section = ParseSectionHeader(file_name, text + 1, line_number);
            continue;
        }

        char* separator = std::strchr(text, '=');

        if (!separator)
        {
            Warningf("%s:%i: ignoring malformed line '%s'", file_name, line_number, text);
            continue;
        }

        *separator = '\0';

        char* key = TrimInPlace(text);
        char* value = TrimInPlace(separator + 1);

        if ((*key == '\0') || (*value == '\0'))
            continue;

        switch (section.SectionType)
        {
            case Section::Type::Invalid: continue;

            case Section::Type::Kind:
            case Section::Type::Texture:
                if (!ApplyGlowKey(section, key, value))
                    Warningf("%s:%i: ignoring '%s = %s' - not a glow setting, or not a value it takes", file_name,
                        line_number, key, value);
                else
                    ++out_applied;
                continue;

            case Section::Type::Plain:
                // An unknown key is a no-op inside cmd_param::set (its lookup simply fails), so a
                // stale or misspelled entry cannot break startup.
                mem::cmd_param::set(key, value);
                ++out_applied;
                continue;
        }
    }

    std::fclose(file);
    return true;
}

// Settings carried over from the old file.
//
// Written INTO the template line each belongs to rather than appended after it: an appended block
// would come later in the file and silently override any later edit to the same setting's own line
// (a [Glow.StreetLamps] intensity would lose to a migrated `lightlamp` below it). Only keys the new
// file has no line for are appended, and those cannot conflict with anything.
struct LegacySetting
{
    char Key[64];
    char Value[192];
    bool Used;
};

static LegacySetting s_legacy[128] {};
static u32 s_legacy_count = 0;

static void ReadLegacySettings()
{
    std::FILE* legacy = std::fopen(kLegacyConfigName, "rb");

    if (!legacy)
        return;

    char line[512];

    while (std::fgets(line, sizeof(line), legacy) && (s_legacy_count < ARTS_SIZE(s_legacy)))
    {
        for (char* c = line; *c; ++c)
        {
            if ((*c == ';') || (*c == '#'))
            {
                *c = '\0';
                break;
            }
        }

        char* text = TrimInPlace(line);
        char* separator = std::strchr(text, '=');

        if ((*text == '\0') || (*text == '[') || !separator)
            continue;

        *separator = '\0';

        const char* key = TrimInPlace(text);
        const char* value = TrimInPlace(separator + 1);

        if ((*key == '\0') || (*value == '\0'))
            continue;

        // Not carried over: its scale changed. It was a damping factor (0.05) for a headlight that was
        // a point light in the wrong place; it is now the brightness of an aimed beam, and the old
        // value would leave headlights all but dark.
        if (EqualNoCase(key, "lighthead"))
            continue;

        LegacySetting& setting = s_legacy[s_legacy_count++];
        arts_strncpy(setting.Key, key, ARTS_TRUNCATE);
        arts_strncpy(setting.Value, value, ARTS_TRUNCATE);
        setting.Used = false;
    }

    std::fclose(legacy);
}

static LegacySetting* FindLegacy(const char* switch_name)
{
    // Last one wins, as it did when the old file was read top to bottom.
    for (u32 i = s_legacy_count; i-- > 0;)
    {
        if (EqualNoCase(s_legacy[i].Key, switch_name))
            return &s_legacy[i];
    }

    return nullptr;
}

static void MarkLegacyUsed(const char* switch_name)
{
    for (u32 i = 0; i < s_legacy_count; ++i)
    {
        if (EqualNoCase(s_legacy[i].Key, switch_name))
            s_legacy[i].Used = true;
    }
}

// The switch a template line sets: the key itself in a plain section, the kind's switch for
// enabled/intensity in a [Glow.<Kind>] section, nothing otherwise.
static const char* SwitchForTemplateLine(const Section& section, const char* key)
{
    if (section.SectionType == Section::Type::Plain)
        return key;

    if (section.SectionType == Section::Type::Kind)
    {
        const KindSwitches switches = SwitchesForKind(section.Kind);

        if (EqualNoCase(key, "enabled"))
            return switches.Enabled;

        if (EqualNoCase(key, "intensity"))
            return switches.Intensity;
    }

    return nullptr;
}

static void WriteTemplate(std::FILE* out)
{
    Section section;
    const char* cursor = kConfigTemplate;

    while (*cursor)
    {
        const char* line_end = std::strchr(cursor, '\n');
        const usize length = line_end ? static_cast<usize>(line_end - cursor) : std::strlen(cursor);

        char line[256] {};
        std::memcpy(line, cursor, std::min<usize>(length, sizeof(line) - 1));
        cursor += length + (line_end ? 1 : 0);

        char parsed[256];
        std::memcpy(parsed, line, sizeof(parsed));
        char* text = TrimInPlace(parsed);

        if (*text == '[')
        {
            if (char* close = std::strchr(text, ']'))
                *close = '\0';

            // Only the section's type and kind matter here; nothing is written to the tuning table,
            // because the template has no [Glow:<TEXTURE>] sections that are not commented out.
            if (StartsWithNoCase(text + 1, "Glow."))
            {
                section.SectionType =
                    agiGlowKindFromName(text + 6, section.Kind) ? Section::Type::Kind : Section::Type::Invalid;
            }
            else
            {
                section.SectionType = Section::Type::Plain;
            }
        }
        else if ((*text != ';') && std::strchr(text, '='))
        {
            char* separator = std::strchr(text, '=');
            *separator = '\0';
            const char* key = TrimInPlace(text);

            if (const char* switch_name = SwitchForTemplateLine(section, key))
            {
                if (const LegacySetting* legacy = FindLegacy(switch_name))
                {
                    std::fprintf(out, "%s = %s\n", key, legacy->Value);
                    MarkLegacyUsed(switch_name);
                    continue;
                }
            }
        }

        std::fputs(line, out);
        std::fputc('\n', out);
    }

    bool header_written = false;

    for (u32 i = 0; i < s_legacy_count; ++i)
    {
        LegacySetting& setting = s_legacy[i];

        if (setting.Used)
            continue;

        if (!header_written)
        {
            header_written = true;
            std::fputs(
                "\n; ---------------------------------------------------------------------------------------------\n"
                "; Carried over from Open1560-Shaders.ini, which this file replaces, and not set anywhere above.\n"
                "; Keys that only fed the retired programmable path are harmless and do nothing.\n"
                "; ---------------------------------------------------------------------------------------------\n"
                "[Migrated]\n",
                out);
        }

        MarkLegacyUsed(setting.Key);
        std::fprintf(out, "%s = %s\n", setting.Key, FindLegacy(setting.Key)->Value);
    }
}

static bool LegacyFileExists()
{
    std::FILE* legacy = std::fopen(kLegacyConfigName, "rb");

    if (!legacy)
        return false;

    std::fclose(legacy);
    return true;
}

static void WriteConfigTemplate(bool migrate)
{
    std::FILE* file = std::fopen(kConfigName, "wb");

    if (!file)
        return;

    if (migrate)
        ReadLegacySettings();

    WriteTemplate(file);
    std::fclose(file);

    Displayf("Wrote default %s%s", kConfigName, migrate ? ", with the settings from Open1560-Shaders.ini" : "");
}

// SETTINGS A FILE DOES NOT HAVE YET
//
// The template is only written when there is no file, so a file from an older build never learns
// about anything added since: its owner cannot see a setting to change it, and "the ini does not
// have that" is indistinguishable from "the ini does nothing". So on load, every setting the template
// has and the file lacks is appended at its default - a whole missing section verbatim, comments and
// all, and a missing key under a repeat of its section header (which the loader takes as a switch
// back to that section). Nothing already in the file is touched or reordered.
//
// Keys in an ordinary section are global switches, so one counts as present if it appears in any
// ordinary section. [Glow...] keys belong to their section. Commented-out template keys (; cone = 25)
// are documentation, not settings, and never appended.
namespace
{
    struct PresentKey
    {
        char Section[64];
        char Key[64];
        bool Glow;
    };
} // namespace

static bool IsGlowSectionName(const char* name)
{
    return StartsWithNoCase(name, "Glow.") || StartsWithNoCase(name, "Glow:");
}

static void UpgradeConfigFile(const char* file_name)
{
    static PresentKey present[512];
    u32 present_count = 0;

    char section_names[64][64] {};
    u32 section_count = 0;

    {
        std::FILE* file = std::fopen(file_name, "rb");

        if (!file)
            return;

        char line[512];
        char current[64] {};

        while (std::fgets(line, sizeof(line), file))
        {
            for (char* c = line; *c; ++c)
            {
                if ((*c == ';') || (*c == '#'))
                {
                    *c = '\0';
                    break;
                }
            }

            char* text = TrimInPlace(line);

            if (*text == '[')
            {
                if (char* close = std::strchr(text, ']'))
                    *close = '\0';

                arts_strncpy(current, text + 1, ARTS_TRUNCATE);

                if (section_count < ARTS_SIZE(section_names))
                    arts_strncpy(section_names[section_count++], current, ARTS_TRUNCATE);

                continue;
            }

            char* separator = std::strchr(text, '=');

            if (!separator || (present_count >= ARTS_SIZE(present)))
                continue;

            *separator = '\0';

            PresentKey& entry = present[present_count++];
            arts_strncpy(entry.Section, current, ARTS_TRUNCATE);
            arts_strncpy(entry.Key, TrimInPlace(text), ARTS_TRUNCATE);
            entry.Glow = IsGlowSectionName(current);
        }

        std::fclose(file);
    }

    auto has_section = [&](const char* name) {
        for (u32 i = 0; i < section_count; ++i)
        {
            if (EqualNoCase(section_names[i], name))
                return true;
        }

        return false;
    };

    auto has_key = [&](const char* section, const char* key) {
        const bool glow = IsGlowSectionName(section);

        for (u32 i = 0; i < present_count; ++i)
        {
            if (!EqualNoCase(present[i].Key, key) || (present[i].Glow != glow))
                continue;

            if (!glow || EqualNoCase(present[i].Section, section))
                return true;
        }

        return false;
    };

    // Walk the template. Lines are gathered per section: a missing section is copied whole, a missing
    // key with the comment lines directly above it.
    static char additions[16384];
    usize additions_len = 0;

    auto append = [&](const char* text, usize length) {
        if ((additions_len + length + 1) < sizeof(additions))
        {
            std::memcpy(additions + additions_len, text, length);
            additions_len += length;
            additions[additions_len++] = '\n';
        }
    };

    char current[64] {};
    bool section_missing = false;
    bool header_written = false;
    const char* comment_start = nullptr;

    for (const char* cursor = kConfigTemplate; *cursor;)
    {
        const char* line_end = std::strchr(cursor, '\n');
        const usize length = line_end ? static_cast<usize>(line_end - cursor) : std::strlen(cursor);
        const char* line_start = cursor;
        cursor += length + (line_end ? 1 : 0);

        char parsed[256] {};
        std::memcpy(parsed, line_start, std::min<usize>(length, sizeof(parsed) - 1));
        char* text = TrimInPlace(parsed);

        if (*text == '[')
        {
            if (char* close = std::strchr(text, ']'))
                *close = '\0';

            arts_strncpy(current, text + 1, ARTS_TRUNCATE);
            section_missing = !has_section(current);
            header_written = false;
            comment_start = nullptr;

            if (section_missing)
            {
                append("", 0);
                append(line_start, length);
                header_written = true;
            }

            continue;
        }

        if (current[0] == '\0')
            continue;

        if (section_missing)
        {
            // The whole section, as the template has it - up to the divider that opens the next part.
            if (!StartsWithNoCase(text, "; ---"))
                append(line_start, length);

            continue;
        }

        if (*text == ';')
        {
            if (!comment_start)
                comment_start = line_start;

            continue;
        }

        const char* separator = std::strchr(text, '=');

        if (!separator)
        {
            comment_start = nullptr;
            continue;
        }

        char key[64] {};
        std::memcpy(key, text, std::min<usize>(static_cast<usize>(separator - text), sizeof(key) - 1));

        if (!has_key(current, TrimInPlace(key)))
        {
            if (!header_written)
            {
                char header[80];
                const i32 header_length = std::snprintf(header, sizeof(header), "[%s]", current);
                append("", 0);
                append(header, static_cast<usize>(std::max(header_length, 0)));
                header_written = true;
            }

            const char* from = comment_start ? comment_start : line_start;
            append(from, static_cast<usize>((line_start + length) - from));
        }

        comment_start = nullptr;
    }

    if (additions_len == 0)
        return;

    std::FILE* file = std::fopen(file_name, "ab");

    if (!file)
        return;

    std::fputs("\n; ---------------------------------------------------------------------------------------------\n"
               "; Added by a newer Open1560: settings this file did not have yet, at their defaults. Edit them\n"
               "; here like any other; they are only ever added once.\n"
               "; ---------------------------------------------------------------------------------------------\n",
        file);
    std::fwrite(additions, 1, additions_len, file);
    std::fclose(file);

    Displayf("%s: added the settings it did not have yet (see the end of the file)", file_name);
}

void agiDX9LoadRemixConfig()
{
    i32 applied = 0;

    UpgradeConfigFile(kConfigName);

    if (!LoadConfigFile(kConfigName, applied))
    {
        // First run with this file. If the old one exists, its settings are copied into the new one
        // rather than read alongside it: two files that can both set a value, in an order nobody can
        // see, is how a setting ends up "not working".
        const bool migrate = LegacyFileExists();

        WriteConfigTemplate(migrate);
        LoadConfigFile(kConfigName, applied);
    }
    else if (LegacyFileExists())
    {
        Displayf(
            "%s is no longer read - its settings live in %s now. It can be deleted.", kLegacyConfigName, kConfigName);
    }

    Displayf("Loaded %s (%i settings, %u glow texture sections)", kConfigName, applied, agiGlowTextureTuningCount());
}

#pragma warning(pop)
