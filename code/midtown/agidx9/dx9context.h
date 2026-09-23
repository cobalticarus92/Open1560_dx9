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

struct IDirect3D9;
struct IDirect3DDevice9;
typedef struct SDL_Window SDL_Window;

// Thin wrapper around IDirect3DDevice9 creation/loss/reset, mirroring the role
// agiGLContext plays for the OpenGL backend (agigl/glcontext.h).
class agiDX9Context
{
public:
    // `adopt`, when non-null, is an existing device this context takes ownership of and resets to
    // the requested size instead of creating a new one. See the parked-device note in dx9pipe.cpp.
    agiDX9Context(
        SDL_Window* window, u32 width, u32 height, bool windowed, bool vsync, IDirect3DDevice9* adopt = nullptr);
    ~agiDX9Context();

    // Hands the device back without releasing it, leaving this context empty. The caller becomes
    // responsible for the reference.
    IDirect3DDevice9* DetachDevice();

    ARTS_NON_COPYABLE(agiDX9Context);

    IDirect3DDevice9* GetDevice() const
    {
        return device_;
    }

    // Call at the start of every frame. Returns false if the device is lost
    // and could not yet be reset (skip the frame).
    bool BeginFrame();

    // Presents the backbuffer. Returns false if the device was lost during Present.
    bool Present();

    // Re-presents the existing device at a new size instead of building a new one. Used when the
    // engine restarts its pipeline to change resolution - see the parked-context note in
    // dx9pipe.cpp for why the device has to survive that.
    void Resize(u32 width, u32 height, bool windowed, bool vsync);

    u32 GetMaxTextureWidth() const
    {
        return max_texture_width_;
    }

    u32 GetMaxTextureHeight() const
    {
        return max_texture_height_;
    }

    u32 GetMaxAnisotropy() const
    {
        return max_anisotropy_;
    }

    bool SupportsHardwareTL() const
    {
        return hardware_tl_;
    }

    // How many textures one draw may sample, and how many blend stages may be chained. They are
    // different numbers and the per-pixel path needs both: its diffuse pass samples two textures
    // across three stages, and its specular pass samples one texture across up to six, because each
    // squaring of N.H costs a stage and buys a doubling of the Blinn exponent.
    u32 GetMaxSimultaneousTextures() const
    {
        return max_simultaneous_textures_;
    }

    u32 GetMaxTextureBlendStages() const
    {
        return max_texture_blend_stages_;
    }

    // D3DTOP_DOTPRODUCT3. This is the whole basis of fixed-function per-pixel lighting - it is the
    // only texture-stage op that produces a dot product per fragment - so the path is gated on it
    // rather than assumed. Universally present on anything that runs D3D9, checked anyway because
    // the failure mode without it is a silently black scene.
    bool SupportsDot3() const
    {
        return supports_dot3_;
    }

    // Size of the fixed-function matrix palette available to indexed vertex blending, or 0 when the
    // device cannot do it. See QueryCaps.
    //
    // Software vertex processing implements the whole of fixed function in the runtime and always
    // offers the full 256-entry palette, whatever the adapter reports, so it is answered separately
    // - that is also the fallback path CreateDevice drops to when a device advertising hardware T&L
    // fails to create one.
    u32 GetSkinPaletteSize() const
    {
        if (!hardware_tl_)
            return 256;

        return (max_vertex_blend_matrix_index_ > 0) ? (max_vertex_blend_matrix_index_ + 1) : 0;
    }

    // How many matrices one vertex may blend BETWEEN. Not what the palette path needs - it binds
    // one matrix per vertex - but a device reporting fewer than one cannot vertex blend at all.
    u32 GetMaxVertexBlendMatrices() const
    {
        return max_vertex_blend_matrices_;
    }

    static void CheckErrors(const char* what, long hr);

private:
    void QueryCaps();
    void CreateDevice();
    void ReleaseDevice();
    bool ResetDevice();

    void* window_ {}; // HWND
    IDirect3D9* d3d_ {};
    IDirect3DDevice9* device_ {};

    u32 width_ {};
    u32 height_ {};
    bool windowed_ {};
    bool vsync_ {};

    bool device_lost_ {};

    u32 max_texture_width_ {};
    u32 max_texture_height_ {};
    u32 max_anisotropy_ {};
    u32 max_simultaneous_textures_ {};
    u32 max_texture_blend_stages_ {};
    u32 max_vertex_blend_matrices_ {};
    u32 max_vertex_blend_matrix_index_ {};
    bool supports_dot3_ {};
    bool hardware_tl_ {};

    // D3DFORMAT - stored as a plain u32 so this header doesn't need the D3D9 headers (see
    // dx9_windows.h) just to declare it; cast back to D3DFORMAT where it's actually used.
    u32 depth_format_ {};
};

inline thread_local agiDX9Context* agiDX9 {};

// True when Direct3DCreate9 was resolved from an RTX Remix bridge client DLL (d3d9_remix.dll or a
// "remix"-named -d3d9dll override). Used to keep screen-space effect draws from looking like "UI"
// to Remix's injection heuristic - see agiDX9Rasterizer::RestoreStateAfterWorldDraw.
bool agiDX9RemixBridgeActive();

// The module Direct3DCreate9 was taken from (an HMODULE, as void* so this header needs no Windows
// headers), or null before the first device. Kept loaded for the life of the process - see
// CreateD3D9(). The Remix API entry point is looked up on it (agidx9/dx9remix.cpp).
void* agiDX9D3D9Module();
