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
// Drives RTX Remix Plus's physical sky ("Numos", rtx.skyMode = 1: Hillaire atmosphere, volumetric
// clouds, stars and moons) and its weather-preset blender from the race's time of day and weather.
// Remix Plus only: it needs the game-value channel, which NVIDIA's bridge does not forward. See
// docs/remix_api_plan.md, section 9, for how each game setting maps onto the sky.

// Before the frame's drawing. Keeps the game's own sky dome switched off while this drives the sky,
// and puts the player's setting back when it stops.
void agiDX9RemixSkyBeginFrame();

// After the frame's drawing, before Present. Pushes the sky when the race's time or weather changes,
// and fires a lightning strike on each thunder clap.
void agiDX9RemixSkyEndFrame();

// Pipeline teardown: the city is going, so give the player's sky setting back and forget what was
// pushed, so the next race pushes its own sky in full.
void agiDX9RemixSkyEndGfx();
