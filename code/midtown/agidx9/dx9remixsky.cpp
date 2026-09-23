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

#include "dx9remixsky.h"

#include "agiworld/quality.h"
#include "agiworld/skyenv.h"

#include "dx9remix.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

define_dummy_symbol(agidx9_dx9remixsky);

// HOW THE REMIX PLUS SKY WORKS, AND WHAT THIS FEEDS IT
//
// Remix Plus (RemixProjGroup/dxvk-remix) replaces the rasterised skybox with a physical sky when
// rtx.skyMode = 1 - "Numos": Hillaire atmospheric scattering, Nubis-style volumetric clouds, stars
// and up to four moons, with the sun injected as a real distant light that the clouds shadow. It is
// controlled through two channels, both reachable from a 32-bit game through its bridge:
//
//   - rtx.* options, through SetConfigVariable: the sun's elevation and rotation, the moons, the
//     stars, and every per-preset weather value.
//   - game values, through SetGameValue: __weather.target names one of twelve weather presets, and
//     a blender crossfades every cloud, atmosphere, fog and precipitation value toward it.
//
// This file maps the race onto both. The sun pose is not part of any weather preset, so it is set
// directly; everything else about the weather comes from the preset the game's weather maps to.
// Keys and values below were read from the Remix Plus source (9aab34bd), not only from its docs,
// which disagree with it in two places - see the moon and preset-key notes below.
//
// Nothing here runs unless the Remix API connected through a Remix Plus bridge and -remixsky is on.

static mem::cmd_param PARAM_remixsky {"remixsky", "Drive RTX Remix Plus's physical sky from the race's time and weather"};

// Which Remix Plus weather preset each of the game's four weathers maps to.
static mem::cmd_param PARAM_remixsky_clear {"remixskyclear", "Remix Plus weather preset for Clear"};
static mem::cmd_param PARAM_remixsky_fog {"remixskyfog", "Remix Plus weather preset for Foggy"};
static mem::cmd_param PARAM_remixsky_rain {"remixskyrain", "Remix Plus weather preset for Raining"};
static mem::cmd_param PARAM_remixsky_snow {"remixskysnow", "Remix Plus weather preset for Snowing"};

static mem::cmd_param PARAM_remixsky_fogmatch {"remixskyfogmatch", "Match Remix fog density to the game's own fog distances"};
static mem::cmd_param PARAM_remixsky_fogdensity {"remixskyfogdensity", "Multiplier on matched Remix fog density"};

static mem::cmd_param PARAM_remixsky_precip {
    "remixprecipitation", "Use Remix Plus's own rain and snow instead of leaving it to the game's particles"};

static mem::cmd_param PARAM_remixsky_lightningsync {
    "remixlightningsync", "Fire Remix lightning on the game's thunder instead of at random"};

static mem::cmd_param PARAM_remixsky_sunrotation {
    "remixsunrotation", "Degrees added to the sun and moon azimuth, to turn the sky to the city"};

namespace
{
    // What this file needs to know about each Remix Plus weather preset, from the
    // WEATHER_PRESET_VALUES_* tables in its rtx_weather.h (9aab34bd).
    //
    //   Transmittance - mean of the preset's volumetric transmittanceColor. The fog's extinction per
    //                   metre is -ln(Transmittance) / transmittanceMeasurementDistanceMeters, so the
    //                   two only mean something together; see MatchedFogDistance.
    //   Lightning     - the preset's own strikes per minute.
    //   Drift         - the "Weather Variation" personality the Remix Plus integration guide
    //                   recommends per preset: how fast and how far the clouds wander.
    struct PresetInfo
    {
        const char* Name;
        f32 Transmittance;
        f32 Lightning;
        f32 DriftSpeed;
        f32 DriftIntensity;
    };

    constexpr PresetInfo kPresets[] {
        {"clear", 0.999f, 0.0f, 0.6f, 0.5f},
        {"partlyCloudy", 0.998f, 0.0f, 1.0f, 1.0f},
        {"overcast", 0.995f, 0.0f, 0.7f, 0.7f},
        {"hazy", 0.965f, 0.0f, 0.8f, 0.6f},
        {"foggy", 0.940f, 0.0f, 0.5f, 0.4f},
        {"drizzle", 0.960f, 0.0f, 1.2f, 1.1f},
        {"rainstorm", 0.883f, 4.0f, 1.6f, 1.4f},
        {"thunderstorm", 0.783f, 12.0f, 2.0f, 1.6f},
        {"snow", 0.980f, 0.0f, 0.9f, 0.8f},
        {"blizzard", 0.950f, 0.0f, 1.8f, 1.5f},
        {"sandstorm", 0.650f, 0.0f, 1.5f, 1.6f},
        {"smoggy", 0.633f, 0.0f, 0.8f, 0.7f},
    };

    const PresetInfo* FindPreset(const char* name)
    {
        for (const PresetInfo& preset : kPresets)
        {
            if (std::strcmp(preset.Name, name) == 0)
                return &preset;
        }

        return nullptr;
    }

    // The game's weather to a Remix Plus preset. Chosen by what the game itself does in each:
    //
    //   Clear -> clear       "Weather: Clear". A few high clouds, crisp air, no fog (the game has
    //                        none either: FogEnd 0).
    //   Fog   -> foggy       The preset Remix Plus calls its headline fog. Also the anchor the other
    //                        weathers' fog is matched against.
    //   Rain  -> rainstorm   The game's rain is a storm - mmRainAudio plays thunder and the sky
    //                        flashes - but a light-fogged one (FogEnd 500, the thinnest of the
    //                        three). rainstorm has lightning and heavy cloud without thunderstorm's
    //                        near-black sky and 60 m visibility.
    //   Snow -> snow         Steady snowfall and a cold, even sky. Not blizzard: the game's snow
    //                        is fogged no harder than its fog.
    //
    // Each is a setting (remixskyclear/fog/rain/snow) for anyone who wants a different mood.
    const char* PresetForWeather(i32 weather)
    {
        switch (weather)
        {
            case 1: return PARAM_remixsky_fog.get_or<const char*>("foggy");
            case 2: return PARAM_remixsky_rain.get_or<const char*>("rainstorm");
            case 3: return PARAM_remixsky_snow.get_or<const char*>("snow");
            default: return PARAM_remixsky_clear.get_or<const char*>("clear");
        }
    }

    // The sun, and at night the moon, for each time of day.
    //
    // Elevation and azimuth are the authored table the retired programmable path was tuned with
    // (dx9shader.cpp, agiDX9ComputeSunLight; docs/remix_api_data_sources.md section 2), not the
    // engine's own sun. The engine's is unusable for a physical sky: fix_sun() only ever sets an
    // elevation - its azimuth global is written once, to zero - so the sun rises and sets in one
    // plane, and Night has no case at all and keeps whatever the previous race left.
    //
    // Azimuth is 0 = +Z, increasing toward +X. That is exactly the convention Remix Plus builds its
    // sun direction with (rtx_atmosphere.cpp: x = cos(e) sin(a), y = sin(e), z = cos(e) cos(a)) for a
    // Y-up world like this one, so the table goes across unchanged.
    //
    // Night is the one real change. The programmable path had no night sky, so it lit night with a
    // dim high "sun" to stand in for the moon. Remix Plus has a real one: the sun goes below the
    // horizon, far enough (-18 degrees, astronomical twilight) that the sky is fully dark and the
    // stars come out, and moon 0 takes the old moonlight direction, full.
    struct TimePreset
    {
        const char* Name;
        f32 SunElevation;
        f32 SunAzimuth;
        bool Moon;
        f32 MoonElevation;
        f32 MoonAzimuth;
        f32 StarBrightness;
    };

    constexpr TimePreset kTimes[4] {
        {"Morning", 18.0f, 95.0f, false, 0.0f, 0.0f, 0.0f}, // low, warm, from the east
        {"Noon", 74.0f, 195.0f, false, 0.0f, 0.0f, 0.0f},   // near overhead
        {"Sunset", 9.0f, 268.0f, false, 0.0f, 0.0f, 0.0f},  // very low, from the west
        {"Night", -18.0f, 205.0f, true, 52.0f, 25.0f, 0.5f}, // sun opposite the moon, below the horizon
    };

    constexpr const char* kWeatherNames[4] {"Clear", "Foggy", "Raining", "Snowing"};

    // Fog matching. Remix Plus's presets are tuned in isolation and disagree with the game about
    // which weather is thickest: its snow is about ten times thinner than its fog, and its rain is
    // thicker than its fog. The game fogs Fog and Snow alike (FogEnd 200) and Rain far more lightly
    // (500). So the extinction is scaled by the game's own ratio, anchored on the one preset built
    // to BE fog: foggy as shipped (transmittance 0.94 over 80 m) stands for the game's daytime Fog,
    // and every other weather is that extinction times 200 / its own FogEnd.
    constexpr f32 kAnchorTransmittance = 0.94f;
    constexpr f32 kAnchorDistance = 80.0f;
    constexpr f32 kAnchorGameFogEnd = 200.0f;

    // Strikes per minute that makes a strike certain on the frame it is set: the runtime draws a
    // Bernoulli trial at (rate / 60) * dt each frame, so this is certain at any dt above ~0.1 ms.
    constexpr const char* kLightningStrikeNow = "1000000.0";

    struct State
    {
        bool SkyOverridden;
        b32 SavedTexturedSky;

        i32 PushedTime;
        i32 PushedWeather;
        const PresetInfo* Preset;
        char PresetName[32];

        u32 ThunderSeen;
        bool LightningSpiked;
        bool WarnedNoGameValues;
    };

    State s_sky {false, 0, -1, -1, nullptr, {}, 0, false, false};
} // namespace

static bool SkyWanted()
{
    return PARAM_remixsky.get_or(true) && agiDX9RemixApiActive();
}

static bool SkyActive()
{
    if (!SkyWanted() || (agiSkyEnv.TimeOfDay < 0))
        return false;

    if (!agiDX9RemixApiHasGameValues())
    {
        if (!s_sky.WarnedNoGameValues)
        {
            s_sky.WarnedNoGameValues = true;
            Displayf("Remix sky: not driven - this bridge has no game-value channel. The physical sky and weather "
                     "presets are Remix Plus features (-remixsky 0 silences this).");
        }

        return false;
    }

    return true;
}

static void SetConfig(const char* key, const char* value)
{
    agiDX9RemixApiSetConfig(key, value);
}

static void SetConfigFloat(const char* key, f32 value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", value);
    agiDX9RemixApiSetConfig(key, text);
}

static void SetConfigBool(const char* key, bool value)
{
    agiDX9RemixApiSetConfig(key, value ? "True" : "False");
}

// Moon options were renamed by Remix Plus's 2026-08-26 subsystem refactor (196a05b5): before it,
// every field carried the moon's index as a suffix (rtx.atmosphere.moon0.enabled0); after it, it does
// not (rtx.atmosphere.moon0.enabled). Its RemixSkyAPI.md still documents the old form. A build could
// be from either side, so both are sent; a runtime ignores the key it does not have.
static void SetMoonConfig(const char* field, const char* value)
{
    char key[96];

    std::snprintf(key, sizeof(key), "rtx.atmosphere.moon0.%s", field);
    agiDX9RemixApiSetConfig(key, value);

    std::snprintf(key, sizeof(key), "rtx.atmosphere.moon0.%s0", field);
    agiDX9RemixApiSetConfig(key, value);
}

static void SetMoonFloat(const char* field, f32 value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", value);
    SetMoonConfig(field, text);
}

// Per-preset option keys are rtx.weather.preset.<preset>.<preset>_<field>. The preset name appears
// twice: the options are declared under the category rtx.weather.preset.<preset> with the field name
// <preset>_<field> (rtx_weather.h, WEATHER_PRESET_RTX_OPTION_FOR). RemixSkyAPI.md's example drops the
// second one, and a key in that form is silently ignored.
static void SetPresetFloat(const char* preset, const char* field, f32 value)
{
    char key[128];
    std::snprintf(key, sizeof(key), "rtx.weather.preset.%s.%s_%s", preset, preset, field);
    SetConfigFloat(key, value);
}

static void SetPresetValue(const char* preset, const char* field, const char* value)
{
    char key[128];
    std::snprintf(key, sizeof(key), "rtx.weather.preset.%s.%s_%s", preset, preset, field);
    agiDX9RemixApiSetConfig(key, value);
}

static f32 WrapDegrees(f32 degrees)
{
    degrees = std::fmod(degrees, 360.0f);
    return (degrees < 0.0f) ? (degrees + 360.0f) : degrees;
}

// The preset's transmittanceMeasurementDistanceMeters that gives the extinction the game's own fog
// distance calls for - see kAnchorTransmittance. 0 when the game has no fog in this weather, in which
// case the preset is left as authored.
static f32 MatchedFogDistance(const PresetInfo& preset, f32 game_fog_end)
{
    if (game_fog_end <= 0.0f)
        return 0.0f;

    const f32 density = std::max(PARAM_remixsky_fogdensity.get_or(1.0f), 0.01f);
    const f32 anchor_extinction = -std::log(kAnchorTransmittance) / kAnchorDistance;
    const f32 extinction = anchor_extinction * (kAnchorGameFogEnd / game_fog_end) * density;

    const f32 distance = -std::log(std::min(preset.Transmittance, 0.9999f)) / extinction;

    // The option's own range (rtx_weather.h: 1 to 2000).
    return std::clamp(distance, 1.0f, 2000.0f);
}

static void PushSky(i32 time_of_day, i32 weather)
{
    const TimePreset& time = kTimes[time_of_day];
    const f32 rotation = PARAM_remixsky_sunrotation.get_or(0.0f);

    // The physical sky, and the sun under this file's control rather than Remix Plus's own day
    // cycle, which when on owns the sun outright (rtx_atmosphere.cpp, getAtmosphereArgs).
    SetConfig("rtx.skyMode", "1");
    SetConfigBool("rtx.atmosphere.timeCycleEnable", false);

    SetConfigFloat("rtx.atmosphere.sunElevation", time.SunElevation);
    SetConfigFloat("rtx.atmosphere.sunRotation", WrapDegrees(time.SunAzimuth + rotation));

    SetMoonConfig("enabled", time.Moon ? "True" : "False");

    if (time.Moon)
    {
        SetMoonFloat("elevation", time.MoonElevation);
        SetMoonFloat("rotation", WrapDegrees(time.MoonAzimuth + rotation));
        SetMoonFloat("phase", 0.5f);
    }

    SetConfigFloat("rtx.atmosphere.starBrightness", time.StarBrightness);

    // The game draws its own rain and snow (asParticles, from mmCullCity's birth rules), as
    // world-space quads Remix already sees. Remix Plus's precipitation on top would double it, so it
    // is off unless asked for - and asking for it means hiding the game's particle textures with
    // Remix's texture tagging (Ignore), or the rain falls twice.
    SetConfigBool("rtx.weather.precipitation.enable", PARAM_remixsky_precip.get_or(false));

    const char* preset_name = PresetForWeather(weather);
    const PresetInfo* preset = FindPreset(preset_name);

    std::snprintf(s_sky.PresetName, sizeof(s_sky.PresetName), "%s", preset_name);
    s_sky.Preset = preset;

    f32 fog_distance = 0.0f;

    if (preset)
    {
        if (PARAM_remixsky_fogmatch.get_or(true))
        {
            fog_distance = MatchedFogDistance(*preset, agiSkyEnv.FogEnd[weather]);

            if (fog_distance > 0.0f)
                SetPresetFloat(preset->Name, "transmittanceMeasurementDistanceMeters", fog_distance);
        }

        // With lightning synced, the preset's own random strikes are switched off and each of the
        // game's thunder claps fires one instead (see agiDX9RemixSkyEndFrame). Without, the preset's
        // authored rate is put back, in case an earlier race synced it to 0.
        if (preset->Lightning > 0.0f)
        {
            if (PARAM_remixsky_lightningsync.get_or(true))
                SetPresetValue(preset->Name, "lightningStrikesPerMinute", "0.0");
            else
                SetPresetFloat(preset->Name, "lightningStrikesPerMinute", preset->Lightning);
        }
    }
    else
    {
        Warningf("Remix sky: '%s' is not a Remix Plus weather preset this build knows; sent as the target "
                 "anyway, without fog matching or drift tuning.",
            preset_name);
    }

    // The preset itself. Drift first, then a short blend so the race opens on its own sky rather than
    // crossfading into it from the menu's, then the target - the order the integration guide gives,
    // so the blender reads all of them on the same frame.
    char text[32];

    std::snprintf(text, sizeof(text), "%.2f", preset ? preset->DriftSpeed : 1.0f);
    agiDX9RemixApiSetGameValue("__weather.drift_speed", text);

    std::snprintf(text, sizeof(text), "%.2f", preset ? preset->DriftIntensity : 1.0f);
    agiDX9RemixApiSetGameValue("__weather.drift_intensity", text);

    agiDX9RemixApiSetGameValue("__weather.blend_seconds", "0.1");
    agiDX9RemixApiSetGameValue("__weather.target", preset_name);

    s_sky.PushedTime = time_of_day;
    s_sky.PushedWeather = weather;
    s_sky.ThunderSeen = agiSkyEnv.ThunderCount;
    s_sky.LightningSpiked = false;

    Displayf("Remix sky: %s, %s -> preset '%s', sun %.0f deg at %.0f%s, fog %s", time.Name, kWeatherNames[weather],
        preset_name, time.SunElevation, WrapDegrees(time.SunAzimuth + rotation), time.Moon ? " (moonlit)" : "",
        (fog_distance > 0.0f) ? "matched to the game's" : "as authored");

    if (fog_distance > 0.0f)
    {
        Displayf("Remix sky: %s fog: game FogEnd %.0f -> %s transmittanceMeasurementDistanceMeters %.1f",
            kWeatherNames[weather], agiSkyEnv.FogEnd[weather], preset_name, fog_distance);
    }
}

void agiDX9RemixSkyBeginFrame()
{
    // Hide the game's sky dome while Remix Plus draws the sky. The dome is a large textured mesh
    // around the camera, and Remix only leaves a draw out of the scene when it is categorised as
    // sky - which MM1's is not, unless someone has tagged its textures. Left in, it would sit between
    // the camera and the whole physical sky, and between the city and the sun.
    //
    // The game already has a switch for exactly this: its "Textured Sky" graphics option. With it
    // off, asRenderWeb::Update does not draw the dome and the screen is cleared instead (game.asm
    // ~180263, ~180561), which Remix ignores. So that is the switch used, and the player's own
    // setting is put back the moment this stops driving the sky. Set every frame while active,
    // because the graphics menu can write it.
    if (SkyActive())
    {
        if (!s_sky.SkyOverridden)
        {
            s_sky.SavedTexturedSky = agiRQ.TexturedSky;
            s_sky.SkyOverridden = true;
        }

        agiRQ.TexturedSky = false;
    }
    else if (s_sky.SkyOverridden)
    {
        agiRQ.TexturedSky = s_sky.SavedTexturedSky;
        s_sky.SkyOverridden = false;
    }
}

void agiDX9RemixSkyEndFrame()
{
    if (!SkyActive())
        return;

    const i32 time_of_day = agiSkyEnv.TimeOfDay;
    const i32 weather = agiSkyEnv.Weather;

    if ((time_of_day != s_sky.PushedTime) || (weather != s_sky.PushedWeather))
    {
        PushSky(time_of_day, weather);
        return;
    }

    // Lightning on the game's thunder.
    //
    // Remix Plus has no call to fire a strike - its "Test Strike" is dev-menu only - but it draws a
    // fresh Bernoulli trial every frame at the target preset's strikesPerMinute, and the blender
    // re-reads the target preset every frame (rtx_weather.cpp, applyBlendedValues). So the rate is
    // raised to certain for one frame and dropped back to 0 on the next. The runtime's own flicker
    // envelope, restrikes and cloud lighting then do the rest.
    if (!s_sky.Preset || (s_sky.Preset->Lightning <= 0.0f) || !PARAM_remixsky_lightningsync.get_or(true))
        return;

    if (s_sky.LightningSpiked)
    {
        SetPresetValue(s_sky.Preset->Name, "lightningStrikesPerMinute", "0.0");
        s_sky.LightningSpiked = false;
    }

    if (agiSkyEnv.ThunderCount != s_sky.ThunderSeen)
    {
        s_sky.ThunderSeen = agiSkyEnv.ThunderCount;
        SetPresetValue(s_sky.Preset->Name, "lightningStrikesPerMinute", kLightningStrikeNow);
        s_sky.LightningSpiked = true;
    }
}

void agiDX9RemixSkyEndGfx()
{
    if (s_sky.SkyOverridden)
    {
        agiRQ.TexturedSky = s_sky.SavedTexturedSky;
        s_sky.SkyOverridden = false;
    }

    // A spike left standing would make the next race's storm strike continuously.
    if (s_sky.LightningSpiked && s_sky.Preset)
        SetPresetValue(s_sky.Preset->Name, "lightningStrikesPerMinute", "0.0");

    s_sky.LightningSpiked = false;
    s_sky.PushedTime = -1;
    s_sky.PushedWeather = -1;
    s_sky.Preset = nullptr;
}
