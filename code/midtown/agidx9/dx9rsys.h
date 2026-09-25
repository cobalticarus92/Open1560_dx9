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

#include "agi/rsys.h"

#include "dx9pipe.h"

// -d3d9legacydepth. Restores the pre-fix behaviour on both sides of the depth-range change:
// BuildProjectionMatrix() folds agiMeshSet::DepthScale/DepthOffset back into the projection, and
// agiDX9Pipeline::BeginFrame() stops forcing them to 0.5/0.5. Defined in dx9rsys.cpp, shared with
// dx9pipe.cpp because the two halves only make sense together. See BuildProjectionMatrix().
extern mem::cmd_param PARAM_d3d9_legacydepth;

class Matrix34;
class agiViewParameters;
struct IDirect3DTexture9;
struct IDirect3DDevice9;

// Per-frame tally of how geometry reaches the device. "World" submissions are model-space vertices
// plus real SetTransform(WORLD/VIEW/PROJECTION) calls - the only kind RTX Remix can reconstruct a
// 3D scene from. "Screen" submissions are CPU-pretransformed XYZRHW vertices, which carry no
// world-space information at all. Reset and reported once per frame by agiDX9Pipeline::EndFrame().
struct agiDX9SubmitCensus
{
    u32 WorldCalls;
    u32 WorldTris;

    // World draws that came from the mesh cache's device buffers rather than DrawIndexedPrimitiveUP.
    // See dx9meshcache.h.
    u32 WorldCachedCalls;
    u32 WorldStaticLitTris;
    u32 WorldUnlitTris;

    // Split by render phase. Screen-space geometry submitted *inside* BeginScene/EndScene is real
    // 3D world content still going out CPU-pretransformed - that is the number that must reach
    // zero. Screen-space geometry submitted after EndScene is HUD, text, and the minimap, which
    // are genuinely 2D and are supposed to stay pretransformed.
    u32 ScreenCalls;
    u32 ScreenTris;
    u32 ScreenTrisInScene;
    u32 ScreenCallsInScene;

    u32 ScreenLineCalls;
    u32 ScreenLines;

    // -ffperpixel. Additive per-fragment sun passes submitted this frame; two per lit world draw
    // when the specular pass is on, one when it is not, zero when the path is off.
    u32 PerPixelPasses;

    // -ghash. Zero unless the switch is on. See agiDX9GHashRecord in dx9rsys.cpp for what is
    // hashed and why the churn number is the one that matters.
    u32 GHashDraws;
    u32 GHashStable;
    u32 GHashNew;
    u32 GHashDistinct;
};

extern agiDX9SubmitCensus agiDX9Census;

// Advances the -ghash frame counter. Called once per frame from agiDX9Pipeline::EndFrame, next to
// the census reset, because "was this hash also seen last frame" needs a frame number to compare.
void agiDX9GHashNextFrame();

// Occupancy of the -ghash table, reported alongside CHURN so a wrap cannot be mistaken for a clean
// scene. The table clears itself and counts a wrap when it fills; wraps > 0 means something is
// minting hashes continuously, whatever CHURN happened to read in the frame that got sampled.
u32 agiDX9GHashTableUsed();
u32 agiDX9GHashTableCapacity();
u32 agiDX9GHashWraps();

// Dumps and resets the per-texture attribution for hash churn and for in-scene screen draws - the
// two places the census reports a number that is only actionable once you know which texture it
// belongs to. Called from the same 120-frame report block.
void agiDX9DumpAttribution();

// Drops the light/material mirror and the render-state cache described in dx9rsys.cpp. Call
// whenever the device stops holding what was last sent to it - creation and loss.
void agiDX9InvalidateLightCache();

// Drops the render-state cache alone. Every state write inside agiDX9Rasterizer goes through that
// cache, so anything OUTSIDE it that writes render state, texture stage state, FVF or a transform
// has to say so here - otherwise the cache keeps answering for a value the device no longer holds
// and the next write that matches it is skipped. See agiDX9WorldStateCache for the full argument.
void agiDX9InvalidateStateCache();

// Drops the render-state cache's record of the bound vertex and index buffer alone. For the world
// mesh cache, when it releases buffers: a new buffer can be created at a released one's address, and
// the record would then skip the bind that switches to it. See dx9meshcache.cpp.
void agiDX9ForgetStreams();

// Drops one texture from the stage-binding cache. Call before releasing an IDirect3DTexture9: the
// cache compares raw pointers, and a freed allocation can be handed straight back out for the next
// texture, at which point a stale entry would report the new texture as already bound and skip the
// SetTexture that would have bound it.
void agiDX9ForgetTexture(IDirect3DTexture9* texture);

// EVERY MIRROR THIS BACKEND KEEPS OF DEVICE STATE, DROPPED IN ONE CALL. Call it immediately after
// any IDirect3DDevice9::Reset() or CreateDevice().
//
// A Reset() puts render states, texture stage states, sampler states, transforms, lights and
// materials all back to the D3D9 defaults, and every cache here is a "have I already sent this?"
// mirror whose entire purpose is to SKIP the call when it believes the device already agrees. So a
// reset the mirrors do not hear about does not merely make them stale - it makes them suppress
// exactly the writes that would have repaired the device, and the state stays wrong until something
// asks for a different value by chance.
//
// The paths that reset are not rare: the menu <-> race transition resets the parked device
// (agiDX9Pipeline::EndGfx), agiDX9Context::Resize resets it, and a lost device recovered in
// BeginFrame() resets it mid-session with no BeginGfx() following to put anything back. What it
// looked like: D3DRS_LIGHTING defaults to TRUE and D3DRS_DITHERENABLE to FALSE, samplers default to
// POINT with WRAP addressing - so after one alt-tab in a race, or on the way back into the menus,
// the scene renders point-filtered with clamped textures tiling, and the three render states
// BeginGfx() writes are dropped because the cache still holds the values it wrote to the old device.
void agiDX9OnDeviceReset(IDirect3DDevice9* device);

class agiDX9Rasterizer final : public agiRasterizer
{
public:
    agiDX9Rasterizer(agiPipeline* pipe);
    ~agiDX9Rasterizer();

    void EndGfx() override;
    i32 BeginGfx() override;

    void BeginGroup() override;
    void EndGroup() override;

    void Verts(agiVtxType type, agiVtx* vertices, i32 vertex_count) override;
    void Points(agiVtxType type, agiVtx* vertices, i32 vertex_count) override;
    void SetVertCount(i32 vertex_count) override;
    void Triangle(i32 i0, i32 i1, i32 i2) override;
    void Line(i32 i0, i32 i1) override;
    void Card(i32 i0, i32 i1) override;
    void Mesh(agiVtxType type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count) override;

    bool MeshWorld(agiWorldVtx* vertices, i32 vertex_count, u16* indices, i32 index_count, const Matrix34& world,
        const Matrix34& view, const agiViewParameters& proj_params, bool static_lighting,
        const agiNativeMaterialFx* fx = nullptr, bool hardware_lighting = true,
        const agiNativeSkinPalette* skin = nullptr) override;

    u32 MaxNativeSkinBones() const override;

    // The world mesh cache - see dx9meshcache.h.
    bool CachesNativeMeshes() override;
    const agiNativeCachedMesh* FindNativeMesh(u64 key) override;
    const agiNativeCachedMesh* StoreNativeMesh(u64 key, const agiWorldVtx* vertices, u32 vertex_count,
        const u16* indices, u32 index_count, const agiNativeMeshBatch* batches, u32 batch_count) override;

    // Runs that restore, but only if a world draw has left the device in its state.
    //
    // MeshWorld() used to restore at the end of every draw, and with 706-1007 world draws a frame
    // that was ~16 device calls each - around 13,000 a frame - spent putting the device back for a
    // CPU-path draw that, between two city meshes, never comes. The restore exists for the screen
    // path's benefit (agiLastState has to keep describing the device truthfully, see the long note
    // in RestoreStateAfterWorldDraw), so it belongs at the boundary where the screen path resumes,
    // not after every submission.
    //
    // Called from DrawMesh(), which is the single funnel every CPU-pretransformed submission passes
    // through, and from EndGroup() so a frame can never end mid-world-state.
    void LeaveWorldState();

    agiDX9Pipeline* Pipe() const
    {
        return static_cast<agiDX9Pipeline*>(agiRefreshable::Pipe());
    }

private:
    void FlushState();

    // Restores the device state MeshWorld() programs. Shared by both rendering pathways.
    void RestoreStateAfterWorldDraw(bool remap_vertex_fog);

    u16* ImmAddIndices(u32 prim_type, u16 count);
    void ImmDraw();

    void DrawMesh(u32 prim_type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count);

    IDirect3DTexture9* current_texture_ {};

    // Set by MeshWorld(), cleared by LeaveWorldState(). See LeaveWorldState().
    bool world_state_active_ {};
    bool world_remap_vertex_fog_ {};

    agiTexEnv tex_env_ {};
};
