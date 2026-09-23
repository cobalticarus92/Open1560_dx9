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

#include "dx9texdef.h"

#include "agi/cmodel.h"
#include "agi/error.h"
#include "agiworld/glowlight.h"
#include "agiworld/texsheet.h"
#include "pcwindis/setupdata.h"
#include "vector7/vector2.h"

#include "dx9context.h"
#include "dx9rsys.h" // agiDX9ForgetTexture

#include "dx9_windows.h"

define_dummy_symbol(agidx9_dx9texdef);

// clang-format off
static constexpr const u32 MultiplyDeBruijnBitPosition[32] = {
    0,  9,  1, 10, 13, 21,  2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
    8, 12, 20, 28, 15, 17, 24,  7, 19, 27, 23,  6, 26,  5, 4, 31
};
// clang-format on

static u32 ilog2(u32 value)
{
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;

    return MultiplyDeBruijnBitPosition[(value * 0x07C4ACDDu) >> 27];
}

static u32 CaluclateMipMapLevels(u32 width, u32 height)
{
    return 1 + ilog2(std::max(width, height));
}

static inline void color_key_to_alpha(u32* pixels, u32 len)
{
    for (u32 i = 0; i < len; ++i)
    {
        pixels[i] = -(-static_cast<i32>(pixels[i]) & 0xFFFFFF);
    }
}

// Determines a D3D9 texture format for a given agiSurfaceDesc, and whether the R/B channels
// need to be swapped on upload to reach it (some source layouts don't map onto a guaranteed-
// supported D3D9 format directly).
static D3DFORMAT PickFormat(const agiSurfaceDesc& surface, bool alpha, bool& needs_swizzle)
{
    needs_swizzle = false;

    switch (surface.PixelFormat.RBitMask)
    {
        case 0xF800: return D3DFMT_R5G6B5;  // 565
        case 0xF00: return D3DFMT_A4R4G4B4; // 4444

        case 0xFF: // R in the low byte ("A8B8G8R8"), not a guaranteed D3D9 format - swizzle to A8R8G8B8
            needs_swizzle = true;
            return alpha ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;

        case 0xFF0000: // R in bits 16-23, already matches A8R8G8B8/X8R8G8B8 byte order
            return alpha ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;

        default: Quitf("Invalid Format");
    }
}

static void CopyPixels(
    u8* dst, u32 dst_pitch, const u8* src, u32 src_pitch, u32 width, u32 height, u32 pixel_size, bool needs_swizzle)
{
    for (u32 y = 0; y < height; ++y)
    {
        const u8* src_row = src + static_cast<usize>(y) * src_pitch;
        u8* dst_row = dst + static_cast<usize>(y) * dst_pitch;

        if (needs_swizzle)
        {
            for (u32 x = 0; x < width; ++x)
            {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
        else
        {
            std::memcpy(dst_row, src_row, static_cast<usize>(width) * pixel_size);
        }
    }
}

// Builds the glow colour grid described in dx9texdef.h. `surface` is A8R8G8B8 here: this only runs
// for AlphaGlow textures, which BeginGfx() has already rebuilt into the pipeline's alpha format.
void agiDX9TexDef::BuildGlowColors(const agiSurfaceDesc& surface)
{
    constexpr u32 kGrid = GlowGridSize;

    Vector3 sums[kGrid * kGrid] {};
    f32 weights[kGrid * kGrid] {};

    for (u32 y = 0; y < surface.Height; ++y)
    {
        const u32* row = reinterpret_cast<const u32*>(static_cast<const u8*>(surface.Surface) + (y * surface.Pitch));
        const u32 cell_y = std::min((y * kGrid) / std::max<u32>(surface.Height, 1), kGrid - 1);

        for (u32 x = 0; x < surface.Width; ++x)
        {
            const u32 pixel = row[x];

            const f32 r = static_cast<f32>((pixel >> 16) & 0xFF) / 255.0f;
            const f32 g = static_cast<f32>((pixel >> 8) & 0xFF) / 255.0f;
            const f32 b = static_cast<f32>(pixel & 0xFF) / 255.0f;
            const f32 a = static_cast<f32>((pixel >> 24) & 0xFF) / 255.0f;

            // Weight by perceptual luminance (and alpha where present). The background of a glow
            // sheet is black by design, so an unweighted mean would report every lamp as near-black.
            const f32 weight = ((0.2126f * r) + (0.7152f * g) + (0.0722f * b)) * std::max(a, 0.05f);

            if (weight <= 0.0f)
                continue;

            const u32 cell = (cell_y * kGrid) + std::min((x * kGrid) / std::max<u32>(surface.Width, 1), kGrid - 1);

            sums[cell] += Vector3 {r, g, b} * weight;
            weights[cell] += weight;
        }
    }

    for (u32 i = 0; i < (kGrid * kGrid); ++i)
    {
        if (weights[i] > 0.0f)
        {
            Vector3 average = sums[i] / weights[i];

            // Normalise to the brightest channel. What matters for a light is its hue, not how dim
            // the sprite happens to be drawn - brightness is carried by the vertex colour/alpha tint
            // and by -d3d9glowpower. Without this, a deep red tail light would emit an almost black
            // red and be indistinguishable from no light at all.
            f32 peak = std::max({average.x, average.y, average.z});

            if (peak > 1e-3f)
                average = average / peak;

            glow_colors_[i] = average;
        }
        else
        {
            glow_colors_[i] = {1.0f, 1.0f, 1.0f};
        }
    }

    glow_colors_valid_ = true;
}

Vector3 agiDX9TexDef::SampleGlowColor(f32 u, f32 v) const
{
    if (!glow_colors_valid_)
        return {1.0f, 1.0f, 1.0f};

    // Card UVs can sit slightly outside [0,1] on the frame edges; wrap rather than clamp so an
    // atlas frame at the far edge still lands in its own cell.
    u -= std::floor(u);
    v -= std::floor(v);

    const u32 x = std::min(static_cast<u32>(u * GlowGridSize), GlowGridSize - 1);
    const u32 y = std::min(static_cast<u32>(v * GlowGridSize), GlowGridSize - 1);

    return glow_colors_[(y * GlowGridSize) + x];
}

i32 agiDX9TexDef::BeginGfx()
{
    if (Surface == nullptr)
    {
        Errorf("Missing SurfaceDesc for '%s' in BeginGfx - trouble is brewing!", Tex.Name);
        return AGI_ERROR_FILE_NOT_FOUND;
    }

    if (Tex.Name[0] != '*' && page_state_ == 0 && cache_handle_ == 0)
    {
        Surface->Reload(Tex.Name, TexSearchPath, Tex.LOD, PackShift, 0, 0, 0);

        Surface->Pitch = Surface->Width * Surface->GetPixelSize();
    }

    if (Pipe()->Context() == nullptr)
    {
        return AGI_ERROR_SUCCESS;
    }

    if (Tex.Props & agiTexProp::AlphaGlow && GetRendererInfo().AdditiveBlending)
        Tex.Flags &= ~agiTexParameters::Alpha;

    SurfaceSize = 0;

    agiSurfaceDesc* surface = Surface.get();

    bool color_key = (Tex.Flags & agiTexParameters::Chromakey) || (Surface->Flags & AGISD_CKSRCBLT);
    bool alpha_glow = (Tex.Props & agiTexProp::AlphaGlow);
    bool mip_maps = !(Tex.Flags & agiTexParameters::NoMipMaps) && (surface->Flags & AGISD_MIPMAPCOUNT);

    if (color_key || alpha_glow)
    {
        Ptr<agiSurfaceDesc> temp_surface =
            as_ptr agiSurfaceDesc::Init(surface->Width, surface->Height, Pipe()->GetAlphaFormat());

        temp_surface->CopyFrom(Surface.get(), 0, &Tex);

        if (color_key)
        {
            ArAssert(temp_surface->PixelFormat.RGBAlphaBitMask == 0xFF000000, "Invalid Surface");

            for (u32 y = 0; y < temp_surface->Height; ++y)
            {
                color_key_to_alpha(
                    reinterpret_cast<u32*>(static_cast<u8*>(temp_surface->Surface) + (y * temp_surface->Pitch)),
                    temp_surface->Width);
            }
        }

        surface = temp_surface.release();
        mip_maps = false;
    }

    // The colour grid SampleGlowColor() reads, which is where a glow light's hue comes from. Only
    // built while the glow harvest is running (the Remix API, agidx9/dx9remix.cpp): nothing else
    // reads it, and it walks every pixel of the texture.
    //
    // A texture loaded before the API connected has no grid, and its lights fall back to the
    // harvested tint (see HasGlowColors). The API connects in the first pipeline's BeginGfx, before
    // any city texture exists, so in practice that is only the front-end's own glows.
    if (alpha_glow && agiGlowHarvestEnabled)
        BuildGlowColors(*surface);

    bool alpha = (Tex.Flags & (agiTexParameters::Alpha | agiTexParameters::Chromakey)) != 0;
    bool needs_swizzle = false;
    D3DFORMAT format = PickFormat(*surface, alpha, needs_swizzle);

    SurfaceSize = 0;

    i32 num_levels =
        mip_maps ? std::clamp<i32>(surface->MipMapCount, 1, CaluclateMipMapLevels(surface->Width, surface->Height)) : 1;

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    HRESULT hr = device->CreateTexture(
        surface->Width, surface->Height, num_levels, 0, format, D3DPOOL_MANAGED, &texture_, nullptr);

    if (FAILED(hr))
        Quitf("IDirect3DDevice9::CreateTexture failed, code %x", static_cast<u32>(hr));

    u32 pixel_size = surface->GetPixelSize();

    for (i32 i = 0; i < num_levels; ++i)
    {
        i32 width = (std::max<i32>) (surface->Width >> i, 1);
        i32 height = (std::max<i32>) (surface->Height >> i, 1);
        i32 src_pitch = surface->Pitch >> i;

        const u8* src = static_cast<const u8*>(surface->Surface) + SurfaceSize;

        IDirect3DSurface9* level = nullptr;
        texture_->GetSurfaceLevel(static_cast<u32>(i), &level);

        D3DLOCKED_RECT locked;

        if (SUCCEEDED(level->LockRect(&locked, nullptr, 0)))
        {
            CopyPixels(static_cast<u8*>(locked.pBits), static_cast<u32>(locked.Pitch), src, static_cast<u32>(src_pitch),
                static_cast<u32>(width), static_cast<u32>(height), pixel_size, needs_swizzle);

            level->UnlockRect();
        }

        level->Release();

        SurfaceSize += src_pitch * height;
    }

    // NOTE: deliberately no SetFilters() call here. BeginGfx() runs lazily from GetHandle(), i.e.
    // partway through an unrelated draw's state setup, and sampler state is global - applying this
    // texture's filters at creation time would clobber whatever texture is actually bound right
    // now. FlushState()/MeshWorld() apply filters and addressing at bind time instead.

    if (num_levels == 1)
    {
        Tex.Flags |= agiTexParameters::NoMipMaps;
    }

    if (surface != Surface.get())
    {
        surface->Unload();

        delete surface;
    }

    if (Tex.Name[0] != '*' && cache_handle_ == 0 && !(Tex.Flags & agiTexParameters::KeepLoaded))
        Surface->Unload();

    page_state_ = 0;
    state_ = 2;

    return AGI_ERROR_SUCCESS;
}

void agiDX9TexDef::EndGfx()
{
    if (texture_)
    {
        // Before the Release, not after. The stage-binding cache in dx9rsys.cpp compares raw
        // pointers, and D3D9 is free to hand this exact address back out for the next texture
        // created - at which point a surviving entry would report the new texture as already bound
        // and skip the SetTexture that would have bound it.
        agiDX9ForgetTexture(texture_);

        texture_->Release();
        texture_ = nullptr;
    }

    if (temp_surface_)
    {
        temp_surface_->Unload();
        temp_surface_ = nullptr;
    }

    touched_ = false;
    state_ = 0;
}

void agiDX9TexDef::Set(Vector2& arg1, Vector2& arg2)
{
    arg1 = arg2;
}

// Sampler state on stage 0 is *global to the device*, not a property of the bound texture, so the
// "have I already set this?" cache has to be global too. It previously lived in per-agiDX9TexDef
// members, which meant each texture only ever compared against what *it* last asked for: bind
// texture A as POINT, then texture B whose own cache still reads LINEAR from its BeginGfx(), and B
// skips the call and silently rasterises with A's POINT filter. Tracking the device's actual last
// applied values fixes that, and lets address mode be handled the same way.
//
// It also has to be dropped whenever the device is reset, for the same reason: a Reset() puts every
// sampler back to the D3D9 defaults (POINT/POINT/NONE, WRAP/WRAP) while these still describe the
// device as it was before, so every call they would have made is skipped and the whole scene
// silently renders point-filtered and wrapping. See agiDX9InvalidateSamplerCache() below.
static u32 LastMinFilter = ~0u;
static u32 LastMagFilter = ~0u;
static u32 LastMipFilter = ~0u;
static u32 LastAddressU = ~0u;
static u32 LastAddressV = ~0u;

void agiDX9InvalidateSamplerCache()
{
    LastMinFilter = ~0u;
    LastMagFilter = ~0u;
    LastMipFilter = ~0u;
    LastAddressU = ~0u;
    LastAddressV = ~0u;
}

void agiDX9TexDef::SetFilters(u32 min, u32 mag, u32 mip)
{
    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    if (min != LastMinFilter)
    {
        LastMinFilter = min;
        device->SetSamplerState(0, D3DSAMP_MINFILTER, min);
    }

    if (mag != LastMagFilter)
    {
        LastMagFilter = mag;
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, mag);
    }

    if (mip != LastMipFilter)
    {
        LastMipFilter = mip;
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, mip);
    }

    // D3D9 defaults both address modes to D3DTADDRESS_WRAP and nothing here ever changed them, so
    // every texture the game asks to *clamp* was tiling instead. The engine tracks this per
    // texture (agiworld/getmesh.cpp clears WrapU/WrapV for agiTexProp::ClampUOrNeither /
    // ClampVOrNeither / ClampBoth), and the OpenGL backend honours it via GL_TEXTURE_WRAP_S/T
    // (agigl/gltexdef.cpp) - this is the D3D9 equivalent. Wrapping a clamp-mode texture makes its
    // edge texels repeat across the surface, which is the road/terrain UV corruption.
    u32 address_u = Tex.ShouldWrapU() ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
    u32 address_v = Tex.ShouldWrapV() ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;

    if (address_u != LastAddressU)
    {
        LastAddressU = address_u;
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, address_u);
    }

    if (address_v != LastAddressV)
    {
        LastAddressV = address_v;
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, address_v);
    }
}

b32 agiDX9TexDef::Lock(agiTexLock& lock)
{
    if (!Surface || !Surface->Surface || !texture_)
        return false;

    if (temp_surface_ == nullptr)
    {
        temp_surface_ = as_ptr agiSurfaceDesc::Init(Surface->Width, Surface->Height, *Surface);

        temp_surface_->CopyFrom(Surface.get(), 0);
    }

    lock.ColorModel = as_raw agiColorModel::FindMatch(temp_surface_.get());
    lock.Width = temp_surface_->Width;
    lock.Height = temp_surface_->Height;
    lock.Pitch = temp_surface_->Pitch;
    lock.Surface = temp_surface_->Surface;
    touched_ = true;

    return true;
}

void agiDX9TexDef::Unlock(agiTexLock& lock)
{
    lock.ColorModel->Release();
    lock = {};
}

b32 agiDX9TexDef::IsAvailable()
{
    return state_ == 2;
}

void agiDX9TexDef::Request()
{
    GetHandle();
}

IDirect3DTexture9* agiDX9TexDef::GetHandle()
{
    if (state_ != 2)
    {
        if (!LockSurfaceIfResident())
        {
            PageInSurface();

            return nullptr;
        }
        if (BeginGfx() == AGI_ERROR_SUCCESS)
        {
            UnlockAndFreeSurface();
        }
        else
        {
            Warningf("agiDX9TexDef::Request - Texture '%s' didn't init properly", Tex.Name);
        }
    }

    if (touched_)
    {
        touched_ = false;

        bool needs_swizzle = false;
        bool alpha = temp_surface_->PixelFormat.RGBAlphaBitMask != 0;
        PickFormat(*temp_surface_, alpha, needs_swizzle);

        IDirect3DSurface9* level0 = nullptr;
        texture_->GetSurfaceLevel(0, &level0);

        D3DLOCKED_RECT locked;

        if (SUCCEEDED(level0->LockRect(&locked, nullptr, 0)))
        {
            CopyPixels(static_cast<u8*>(locked.pBits), static_cast<u32>(locked.Pitch),
                static_cast<const u8*>(temp_surface_->Surface), temp_surface_->Pitch, temp_surface_->Width,
                temp_surface_->Height, temp_surface_->GetPixelSize(), needs_swizzle);

            level0->UnlockRect();
        }

        level0->Release();
    }

    return texture_;
}
