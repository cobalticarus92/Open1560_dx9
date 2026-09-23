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
// The race's time of day and weather, as the renderer needs them to drive a sky of its own - RTX
// Remix Plus's physical sky (agidx9/dx9remixsky.cpp). Pushed down by mmCullCity rather than read
// out of MMSTATE by the renderer, the same way agiNativeCitySphereMap is: agiworld does not depend
// on mmcity, and a value that only exists while a city is loaded is best owned by the city.

struct agiSkyEnvironment
{
    // mmTimeOfDay and mmWeather (mmcityinfo/state.h), or -1 while no city is loaded. Set by
    // mmCullCity::Init, cleared by agiDX9Pipeline::EndGfx along with the rest of the city state.
    i32 TimeOfDay {-1};
    i32 Weather {-1};

    // The game's own fog distance for each weather at this time of day, from mmEnvSetup
    // (mmcity/cullcity.cpp), in world units. 0 means that weather has no fog. All four, not just the
    // current one, because what matters for matching another renderer's fog is the RATIO between
    // weathers: the game fogs Fog and Snow at 200 and Rain at 500, and a sky system that disagrees
    // about which of those is thickest is wrong however its absolute numbers are tuned.
    f32 FogEnd[4] {};

    // Thunder claps heard since the process started. Counted by mmCullCity::Cull off mmSky's flash
    // flag (see mmSky::DoFlash): a counter rather than a flag, so a consumer that samples once a frame
    // can tell a new clap from the last one without having to clear anything.
    u32 ThunderCount {};
};

inline agiSkyEnvironment agiSkyEnv {};
