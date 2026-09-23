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

#include <cstdio>
#include <cstring>

// std::fopen is deprecated by MSVC and this build is /WX. The alternatives are fopen_s (Annex K) or
// routing through the engine's Stream layer - but that layer is sandboxed by HierAllowPath, which
// is not even configured yet at the point this runs, and cannot write files. Reading and writing one
// small config file in the game directory at startup is not the unsafety the deprecation is aimed
// at, so suppress it locally. Same treatment as std::getenv in agiworld/meshrend.cpp.
#pragma warning(push)
#pragma warning(disable : 4996)

define_dummy_symbol(agidx9_dx9config);

static constexpr const char* kConfigName = "Open1560-Shaders.ini";

// Written verbatim when the file does not exist. Every value shown is the built-in default, so the
// generated file is a no-op until something is edited - a user can uncomment and change one line
// without having to work out what the rest were.
static constexpr const char* kConfigTemplate =
    "; Open1560 - shader and lighting configuration\n"
    ";\n"
    "; Every key here is also a command line switch of the same name, and the command line WINS\n"
    "; over this file - so you can override a setting for one run without editing anything.\n"
    ";\n"
    "; NOTE: the programmable path (Pathway B) is no longer wired up, and -d3d9shaders is gone with\n"
    "; it. The keys below that only fed it are inert and are kept so an existing file does not start\n"
    "; warning about unknown keys. Fixed function is the path NVIDIA RTX Remix can reconstruct a\n"
    "; scene from - a vertex shader is opaque to it - which is why the work went that way.\n"
    ";\n"
    "; Values shown are the defaults. Delete this file to regenerate it.\n"
    "\n"
    "[Renderer]\n"
    "; Render the scene into an offscreen target and blit it back. This is the render-target\n"
    "; framework that shadow maps and post-processing are built on; on its own it changes nothing\n"
    "; you can see, and it is not compatible with Remix for the same reason as the line above.\n"
    "d3d9scenetarget = 0\n"
    "; Shader quality tier for the programmable path. The lit pixel shader is almost pure per-pixel\n"
    "; arithmetic, and it - not the draw-call count - is what costs frames: measured at 3 FPS\n"
    "; against 54 for fixed function on the same scene, with clustered lights already off.\n"
    ";   2  everything: three specular directionals, specular point lights, reflections everywhere\n"
    ";   1  sun keeps its highlight, fills go diffuse-only, reflections narrow to vehicles\n"
    ";   0  diffuse only, no environment term\n"
    "; Costs 327 / 249 / 158 of the 512 ps_3_0 instruction slots respectively.\n"
    "d3d9quality = 2\n"
    "\n"
    "[Sun and reflections]\n"
    "; Drive the sun direction and colour from the time-of-day and weather presets rather than the\n"
    "; engine's own, which only ever moves the sun in one axis and has no case for Night at all.\n"
    "d3d9sun = 1\n"
    "; Strength of environment reflections taken from the sky probe. 0 disables them and falls back\n"
    "; to the flat hemisphere term.\n"
    "d3d9reflect = 1.0\n"
    "\n"
    "[Glow light kinds]\n"
    "; Which glow sprites emit light. 0 removes that kind from the scene entirely - it costs no\n"
    "; light slot and no grid entry - rather than merely dimming it. The flare itself still draws;\n"
    "; these control what a glow EMITS, which in the original engine was nothing at all.\n"
    "; Tail and brake are one setting because both draw the same FXLTGLOWRED sheet with no state\n"
    "; flag between them. Reverse lamps read as neutral whites and fall under 'generic'.\n"
    "glowheadlights = 1\n"
    "glowvehiclelights = 1\n"
    "glowtrafficlights = 1\n"
    "glowstreetlamps = 1\n"
    "glowgenericlights = 1\n"
    "\n"
    "[Tonemapping]\n"
    "; ACES filmic curve. Worth having on once glow lights are active, because they are genuine HDR\n"
    "; sources whose sum near a lamp exceeds 1.0 and would otherwise clip to flat white.\n"
    "d3d9tonemap = 1\n"
    "; Overall exposure multiplier applied before the curve.\n"
    "d3d9exposure = 1.0\n"
    "\n"
    "[Fog]\n"
    "; Extra exponential falloff with altitude, on top of the engine's own linear distance fog.\n"
    "; Fixed-function fog cannot express this at all (it is purely radial), so a street and a tower\n"
    "; roof at equal distance get equal fog without it. 0 disables, ~0.003 is a subtle start.\n"
    "d3d9heightfog = 0.0\n"
    "\n"
    "[Lightning]\n"
    "; Brightness of the rain-storm lightning flash. The original only swaps the sky texture for a\n"
    "; frame; this is what makes the strike light the city itself.\n"
    "d3d9flashpower = 4.0\n"
    "\n"
    "[GlowLights]\n"
    "; Real point lights emitted from the game's own light-flare sprites - street lamps, traffic\n"
    "; signals, headlights, tail lights, coronas. Turning this off leaves the flares drawn but\n"
    "; emitting nothing, which is the original behaviour.\n"
    "d3d9glowlights = 1\n"
    "; Master brightness for all of them. The per-kind multipliers below scale on top of this.\n"
    "d3d9glowpower = 1.5\n"
    "; Flare half-extent to light reach, in world units, and the smallest reach any light may have.\n"
    "; The engine records nothing about how far a lamp throws - only how big its flare is drawn - so\n"
    "; these convert one to the other. Emitted intensity scales with the SQUARE of reach, so raising\n"
    "; either of these brightens lights as well as widening them.\n"
    "glowreachscale = 14.0\n"
    "glowreachmin = 20.0\n"
    "\n"
    "[Clustering]\n"
    "; Point lights are looked up per pixel from a wrapping world-space grid, which is what allows\n"
    "; up to 256 of them at once instead of the 16 that fitted in shader constant registers. Cell\n"
    "; size trades precision against how many lights land in one cell: smaller cells cull better but\n"
    "; overflow their 16-light capacity sooner near a dense junction. Roughly one lamp's reach is a\n"
    "; sensible value.\n"
    "d3d9cellsize = 24.0\n"
    "; Specular response from point lights. Every light gets the full Cook-Torrance term now that the\n"
    "; loop is a real loop rather than an unrolled one; turn this off to trade the highlight for a\n"
    "; cheaper shader on weaker hardware.\n"
    "d3d9lightspec = 1\n"
    "\n"
    "[LightKinds]\n"
    "; Per-kind intensity. These are separated because the sprites are not one population: a street\n"
    "; lamp exists to light a street, a tail light exists to be seen. Classification is by glow\n"
    "; texture name and RELATIVE colour saturation - see agiClassifyGlowIntensity in\n"
    "; agiworld/meshrend.cpp. The renderer re-runs it against the colour actually sampled out of the\n"
    "; flare texture, so a lamp drawn with a white vertex colour over a warm sheet classifies warm.\n"
    ";\n"
    "; Headlight cones. Kept very low by default: the cone is a large mesh whose centre sits metres\n"
    "; ahead of the bonnet, so as a point light it washes the road from the wrong place and pops\n"
    "; with the LOD that draws it. 0 disables the light entirely (the sprite still renders).\n"
    "lighthead = 0.05\n"
    "; Tail, brake and reverse lights.\n"
    "lightvehicle = 1.25\n"
    "; Traffic signals.\n"
    "lighttraffic = 2.0\n"
    "; Street lamps and other static illumination. Identified as unsaturated AND warm - every lamp\n"
    "; in this game is incandescent or sodium, so its blue channel sits well under its red.\n"
    "lightlamp = 10.0\n"
    "; Neutral-white glows that are not street lamps: reverse lamps, generic coronas. Split out\n"
    "; because 'not a pure hue' on its own was handing these the full street-lamp budget.\n"
    "lightgeneric = 1.0\n"
    "\n"
    "[RemixAPI]\n"
    "; Send the game's glow sprites - street lamps, traffic signals, tail and brake lights - to RTX\n"
    "; Remix as real lights through the Remix API. Needs the Remix bridge with\n"
    "; `exposeRemixApi = True` in its bridge.conf. The Glow light kinds, LightKinds and GlowLights\n"
    "; reach settings above apply to these lights. See docs/remix_api_plan.md.\n"
    "remixapi = 0\n"
    "; Overall brightness, before the per-kind multipliers.\n"
    "remixlightpower = 1.5\n"
    "; Emitter size in world units. Only changes shadow softness, not brightness.\n"
    "remixlightradius = 0.15\n"
    "; Most lights sent per frame. The brightest are kept.\n"
    "remixmaxlights = 192\n"
    "; Headlight cones too. Off: the cone centre sits metres ahead of the car.\n"
    "remixheadlights = 0\n"
    "; Remix options (rtx.conf keys) set through the API once it connects: key=value|key=value\n"
    "; remixconfig = rtx.fallbackLightMode=0\n"
    "\n"
    "[RemixSky]\n"
    "; RTX Remix Plus only: drive its physical sky from the race's time of day and weather - sun,\n"
    "; moon and stars, volumetric clouds, and a weather preset per game weather. Hides the game's own\n"
    "; sky dome (through its Textured Sky option) while it runs. See docs/remix_api_plan.md.\n"
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
    "[Geometry]\n"
    "; Rebuild smooth vertex normals for the hardware path.\n"
    ";\n"
    "; The engine stores normals as an index into a 198-entry direction table - about 26 degrees\n"
    "; apart - which is coarse enough that all three corners of a facet often quantise to the SAME\n"
    "; normal. Interpolating a constant gives a constant, so low-poly bodywork shades flat no matter\n"
    "; how per-pixel the lighting is. This re-averages them in float, with a hard-edge threshold so\n"
    "; boxes stay boxes.\n"
    "smoothnormals = 1\n"
    "\n"
    "[Debug]\n"
    "; Log each glow texture as it is first harvested, with its position, radius and tint. Useful\n"
    "; for working out why a particular light is or is not showing up.\n"
    "glowdebug = 0\n"
    "; Depth bias for hardware-transformed geometry. Should not be needed any more.\n"
    "d3d9depthbias = 0.0\n"
    "; Add a specular term to the static city lighting rig. Off by default because the original rig\n"
    "; has no specular concept at all, and a per-vertex approximation of one on huge flat facades\n"
    "; reads as wrong.\n"
    "d3d9specular = 0\n";

static void WriteConfigTemplate()
{
    std::FILE* file = std::fopen(kConfigName, "wb");

    if (!file)
        return;

    std::fwrite(kConfigTemplate, 1, std::strlen(kConfigTemplate), file);
    std::fclose(file);

    Displayf("Wrote default %s", kConfigName);
}

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

void agiDX9LoadShaderConfig()
{
    std::FILE* file = std::fopen(kConfigName, "rb");

    if (!file)
    {
        WriteConfigTemplate();
        return;
    }

    char line[512];
    i32 applied = 0;

    while (std::fgets(line, sizeof(line), file))
    {
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

        // Section headers carry no meaning here - every key is a globally-named cmd_param, and the
        // sections exist purely to make the file readable. Skipping them rather than rejecting the
        // file keeps it tolerant of reorganisation.
        if ((*text == '\0') || (*text == '['))
            continue;

        char* separator = std::strchr(text, '=');

        if (!separator)
        {
            Warningf("%s: ignoring malformed line '%s'", kConfigName, text);
            continue;
        }

        *separator = '\0';

        char* key = TrimInPlace(text);
        char* value = TrimInPlace(separator + 1);

        if ((*key == '\0') || (*value == '\0'))
            continue;

        // An unknown key is a no-op inside cmd_param::set (its lookup simply fails), so a stale or
        // misspelled entry cannot break startup - but it should not do so silently either.
        mem::cmd_param::set(key, value);
        ++applied;
    }

    std::fclose(file);

    Displayf("Loaded %s (%i settings)", kConfigName, applied);
}

#pragma warning(pop)
