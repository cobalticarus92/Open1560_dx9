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
// Wet roads for RTX Remix, through the Remix API: a puddle-and-damp layer laid over the road
// surface while it rains, which the path tracer then reflects the city in.
//
// Remix Plus has no wetness of its own, and a D3D9 draw has no way to ask for it, so this adds
// geometry: every road piece the game draws gets a twin, created once per race through the API -
// the same surface, lifted a hair, textured in WORLD space with a puddle mask the game generates
// from noise, and drawn as a static decal. Where the mask says puddle, the decal is near-black and
// mirror-smooth; where it says damp, it is a thin darkening with a lower roughness; elsewhere it
// is fully transparent. How much of the road is wet follows the race's weather.
//
// See the note at the top of the .cpp for how each piece works and why it is built that way.

struct agiWorldVtx;
class Matrix34;

// Frame bookkeeping. Called from agiDX9Pipeline::BeginFrame.
void agiDX9RemixWetBeginFrame();

// A road or ground draw on the hardware path, with its model-space vertices and world matrix.
// Called from agiDX9Rasterizer::MeshWorld for draws agiMeshSet::DrawLitEnv flags as ground.
void agiDX9RemixWetGround(
    const agiWorldVtx* vertices, i32 vertex_count, const u16* indices, i32 index_count, const Matrix34& world);

// Destroys every mesh and the material, and puts back the global roughness it lowered. Called
// when the pipeline is torn down: the race these meshes describe is over.
void agiDX9RemixWetEndGfx();

// One census line. Called from the pipeline's 120-frame census.
void agiDX9RemixWetLogStats(u32 frame);
