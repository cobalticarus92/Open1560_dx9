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
// The world mesh cache: agiDX9Rasterizer's side of agiRasterizer::FindNativeMesh/StoreNativeMesh.
//
// Every world mesh used to be rebuilt on the CPU by agiMeshSet::DrawNativeTransform and handed to
// the device with DrawIndexedPrimitiveUP on every draw - and once per texture batch, so a building
// with six textures sent its whole vertex array six times. The city is the same from one frame to the
// next, so almost all of that work redoes last frame's. Under RTX Remix it is worse: every one of
// those bytes is copied across the 32-to-64-bit bridge and hashed again by the runtime.
//
// This keeps a mesh once it has been drawn twice: a CPU copy of what the build produced (the
// submission path still reads it - glow harvest, the wet-road layer, the second passes) and a
// managed vertex and index buffer with the same bytes, which the draw then comes from. Remix sees
// the same vertex range and the same index values either way, so its geometry hashes do not change.

#include "agi/rsys.h"

struct IDirect3DDevice9;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;

// -d3d9meshcache (on by default).
bool agiDX9MeshCacheEnabled();

// See agiRasterizer::FindNativeMesh / StoreNativeMesh.
const agiNativeCachedMesh* agiDX9MeshCacheFind(u64 key);
const agiNativeCachedMesh* agiDX9MeshCacheStore(u64 key, const agiWorldVtx* vertices, u32 vertex_count,
    const u16* indices, u32 index_count, const agiNativeMeshBatch* batches, u32 batch_count);

// The device buffers for a cached mesh, created on first use. False if they cannot be made, and the
// caller then draws from the mesh's CPU arrays instead.
bool agiDX9MeshCacheBuffers(
    IDirect3DDevice9* device, const agiNativeCachedMesh& mesh, IDirect3DVertexBuffer9** vb, IDirect3DIndexBuffer9** ib);

// Advances the frame count and evicts what has gone unused when over budget. Nothing is evicted at
// any other time, which is what keeps a returned mesh valid for the rest of the frame. Called from
// agiDX9Pipeline::BeginFrame.
void agiDX9MeshCacheBeginFrame();

// Frees everything, device buffers included. Called from agiDX9Pipeline::EndGfx.
void agiDX9MeshCacheReleaseAll();

// One line for the 120-frame census: this frame's world draws, and how many came from the cache's
// buffers, plus the cache's own counts since the last line.
void agiDX9MeshCacheLogStats(u32 frame, u32 cached_draws, u32 world_draws);
