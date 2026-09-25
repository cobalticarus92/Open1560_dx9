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

define_dummy_symbol(agiworld_meshrend);

#include "meshrend.h"

#include "agi/pipeline.h"
#include "agi/rsys.h"
#include "agi/texdef.h"
#include "agi/viewport.h"
#include "agisw/swrend.h"
#include "agiworld/cardworld.h"
#include "agiworld/glowlight.h"
#include "agiworld/glowtune.h"
#include "agiworld/meshlight.h"
#include "agiworld/packnorm.h"
#include "agiworld/quality.h"
#include "agiworld/texsheet.h"
#include "agiworld/texsort.h"
#include "core/podarray.h"
#include "data7/b2f.h"
#include "data7/utimer.h"
#include "dyna7/gfx.h"
#include "memory/alloca.h"
#include "pcwindis/setupdata.h"
#include "vector7/matrix34.h"
#include "vector7/matrix44.h"

#include <cstdlib>

// #ifdef ARTS_ENABLE_KNI
// #    define CLIP_ALL_TO_SCREEN
// #endif

f32 agiMeshSet::DepthOffset = 0.5f;
f32 agiMeshSet::DepthScale = 0.5f;

i32 agiMeshSet::EyePlaneCount = 0;
i32 agiMeshSet::EyePlanesHit = 0;
Vector3 agiMeshSet::EyePos {};

b32 agiMeshSet::FlipX = false;
f32 agiMeshSet::FogValue = 0.0f;
b32 agiMeshSet::MirrorMode = false;
b32 agiMeshSet::AllowEyeBackfacing = false;

Vector3 agiMeshSet::LocPos {};
Matrix34 agiMeshSet::M {};

f32 agiMeshSet::HalfHeight = 0.0f;
f32 agiMeshSet::HalfWidth = 0.0f;
f32 agiMeshSet::MaxX = 0.0f;
f32 agiMeshSet::MaxY = 0.0f;
f32 agiMeshSet::MinX = 0.0f;
f32 agiMeshSet::MinY = 0.0f;
f32 agiMeshSet::OffsX = 0.0f;
f32 agiMeshSet::OffsY = 0.0f;
f32 agiMeshSet::ProjZW = 0.0f;
f32 agiMeshSet::ProjZZ = 0.0f;

u32 agiMeshSet::MtxSerial = 0;
u32 agiMeshSet::ViewSerial = 0;

alignas(64) u8 agiMeshSet::codes[16384];
alignas(64) i16 agiMeshSet::firstFacet[256];
alignas(64) u8 agiMeshSet::fogout[16384];
alignas(64) i16 agiMeshSet::indexCounts[256];
alignas(64) i16 agiMeshSet::nextFacet[16384];
alignas(64) Vector4 agiMeshSet::out[16384];
alignas(64) i16 agiMeshSet::vertCounts[256];

struct CV
{
    f32 x {}, y {}, z {}, w {}; // screen position
    f32 map[3] {};              // source vertex interpolation
    u8 fog {};                  // vertex fog
    u8 idx[3] {};               // source vertex indices

    constexpr CV() = default;

    CV(const Vector4& pos, f32 map_0, f32 map_1, f32 map_2)
        : x(pos.x)
        , y(pos.y)
        , z(pos.z)
        , w(pos.w)
        , map {map_0, map_1, map_2}
    {}

    CV(const Vector2& pos, f32 z, f32 w, f32 map_0, f32 map_1, f32 map_2, i32 idx_0, i32 idx_1, i32 idx_2)
        : x(pos.x)
        , y(pos.y)
        , z(z)
        , w(w)
        , map {map_0, map_1, map_2}
        , idx {static_cast<u8>(idx_0), static_cast<u8>(idx_1), static_cast<u8>(idx_2)}
    {}
};

check_size(CV, 0x20);

struct CT
{
    u32 Index;
    u32 Count;
    u32 Tri[3];
    CT* Next;
};

check_size(CT, 0x18);

static u32 ClippedVertCount = 0;
static u32 ClippedTriCount = 0;

// ?ClippedVerts@@3PAUCV@@A
ARTS_EXPORT CV ClippedVerts[4096];

// ?ClippedTris@@3PAUCT@@A
static CT ClippedTris[1024];

// ?ClippedTextures@@3PAPAUCT@@A
ARTS_EXPORT CT* ClippedTextures[512];

static bool OnlyZClip = false;

// ?ShadowMatrix@@3VMatrix44@@A
ARTS_IMPORT extern Matrix44 ShadowMatrix;

u32 ClipMask = MESH_CLIP_ANY;

void SetClipMode(b32 mask_only_z)
{
    ClipMask = mask_only_z ? (MESH_CLIP_NZ | MESH_CLIP_PZ) : MESH_CLIP_ANY;
    OnlyZClip = true;
}

// ?ClipPX@@YIXAAUCV@@0@Z
static void ARTS_FASTCALL ClipPX(CV& v0, CV& v1)
{
    f32 dx = v1.x - v0.x;
    f32 dy = v1.y - v0.y;
    f32 dz = v1.z - v0.z;
    f32 dw = v1.w - v0.w;

    f32 dtu = v1.map[0] - v0.map[0];
    f32 dtv = v1.map[1] - v0.map[1];
    f32 dtw = v1.map[2] - v0.map[2];

    f32 t = -(v0.w - v0.x) / (dw - dx);

    v0.y += t * dy;
    v0.z += t * dz;
    v0.w += t * dw;
    v0.x = v0.w;

    v0.map[0] += t * dtu;
    v0.map[1] += t * dtv;
    v0.map[2] += t * dtw;

    ++STATS.VertsClip;
}

// ?ClipNX@@YIXAAUCV@@0@Z
static void ARTS_FASTCALL ClipNX(CV& v0, CV& v1)
{
    f32 dx = v1.x - v0.x;
    f32 dy = v1.y - v0.y;
    f32 dz = v1.z - v0.z;
    f32 dw = v1.w - v0.w;

    f32 dtu = v1.map[0] - v0.map[0];
    f32 dtv = v1.map[1] - v0.map[1];
    f32 dtw = v1.map[2] - v0.map[2];

    f32 t = -(v0.w + v0.x) / (dw + dx);

    v0.y += t * dy;
    v0.z += t * dz;
    v0.w += t * dw;
    v0.x = -v0.w;

    v0.map[0] += t * dtu;
    v0.map[1] += t * dtv;
    v0.map[2] += t * dtw;

    ++STATS.VertsClip;
}

// ?ClipPY@@YIXAAUCV@@0@Z
static void ARTS_FASTCALL ClipPY(CV& v0, CV& v1)
{
    f32 dx = v1.x - v0.x;
    f32 dy = v1.y - v0.y;
    f32 dz = v1.z - v0.z;
    f32 dw = v1.w - v0.w;

    f32 dtu = v1.map[0] - v0.map[0];
    f32 dtv = v1.map[1] - v0.map[1];
    f32 dtw = v1.map[2] - v0.map[2];

    f32 t = -(v0.w - v0.y) / (dw - dy);

    v0.x += t * dx;
    v0.z += t * dz;
    v0.w += t * dw;
    v0.y = v0.w;

    v0.map[0] += t * dtu;
    v0.map[1] += t * dtv;
    v0.map[2] += t * dtw;

    ++STATS.VertsClip;
}

// ?ClipNY@@YIXAAUCV@@0@Z
static void ARTS_FASTCALL ClipNY(CV& v0, CV& v1)
{
    f32 dx = v1.x - v0.x;
    f32 dy = v1.y - v0.y;
    f32 dz = v1.z - v0.z;
    f32 dw = v1.w - v0.w;

    f32 dtu = v1.map[0] - v0.map[0];
    f32 dtv = v1.map[1] - v0.map[1];
    f32 dtw = v1.map[2] - v0.map[2];

    f32 t = -(v0.w + v0.y) / (dw + dy);

    v0.x += t * dx;
    v0.z += t * dz;
    v0.w += t * dw;
    v0.y = -v0.w;

    v0.map[0] += t * dtu;
    v0.map[1] += t * dtv;
    v0.map[2] += t * dtw;

    ++STATS.VertsClip;
}

// ?ClipPZ@@YIXAAUCV@@0@Z
static void ARTS_FASTCALL ClipPZ(CV& v0, CV& v1)
{
    f32 dx = v1.x - v0.x;
    f32 dy = v1.y - v0.y;
    f32 dz = v1.z - v0.z;
    f32 dw = v1.w - v0.w;

    f32 dtu = v1.map[0] - v0.map[0];
    f32 dtv = v1.map[1] - v0.map[1];
    f32 dtw = v1.map[2] - v0.map[2];

    f32 t = -(v0.w - v0.z) / (dw - dz);

    v0.x += t * dx;
    v0.y += t * dy;
    v0.w += t * dw;
    v0.z = v0.w;

    v0.map[0] += t * dtu;
    v0.map[1] += t * dtv;
    v0.map[2] += t * dtw;

    ++STATS.VertsClip;
}

// ?ClipNZ@@YIXAAUCV@@0@Z
static void ARTS_FASTCALL ClipNZ(CV& v0, CV& v1)
{
    f32 dx = v1.x - v0.x;
    f32 dy = v1.y - v0.y;
    f32 dz = v1.z - v0.z;
    f32 dw = v1.w - v0.w;

    f32 dtu = v1.map[0] - v0.map[0];
    f32 dtv = v1.map[1] - v0.map[1];
    f32 dtw = v1.map[2] - v0.map[2];

    f32 t = -(v0.w + v0.z) / (dw + dz);

    v0.x += t * dx;
    v0.y += t * dy;
    v0.w += t * dw;
    v0.z = -v0.w;

    v0.map[0] += t * dtu;
    v0.map[1] += t * dtv;
    v0.map[2] += t * dtw;

    ++STATS.VertsClip;
}

// ?ClipNX@@YAHPAUCV@@0H@Z
static i32 ClipNX(CV* output, CV* input, i32 count)
{
    i32 done = 0;
    i32 prev = count - 1;
    bool prev_clipped = -input[prev].x > input[prev].w;

    for (i32 i = 0; i < count; ++i)
    {
        bool clipped = -input[i].x > input[i].w;

        if (clipped != prev_clipped)
        {
            output[done] = input[clipped ? i : prev];
            ClipNX(output[done], input[clipped ? prev : i]);
            ++done;
        }

        if (!clipped)
        {
            output[done] = input[i];
            ++done;
        }

        prev_clipped = clipped;
        prev = i;
    }

    return done;
}

// ?ClipPX@@YAHPAUCV@@0H@Z
static i32 ClipPX(CV* output, CV* input, i32 count)
{
    i32 done = 0;
    i32 prev = count - 1;
    bool prev_clipped = input[prev].x > input[prev].w;

    for (i32 i = 0; i < count; ++i)
    {
        bool clipped = input[i].x > input[i].w;

        if (clipped != prev_clipped)
        {
            output[done] = input[clipped ? i : prev];
            ClipPX(output[done], input[clipped ? prev : i]);
            ++done;
        }

        if (!clipped)
        {
            output[done] = input[i];
            ++done;
        }

        prev_clipped = clipped;
        prev = i;
    }

    return done;
}

// ?ClipNY@@YAHPAUCV@@0H@Z
static i32 ClipNY(CV* output, CV* input, i32 count)
{
    i32 done = 0;
    i32 prev = count - 1;
    bool prev_clipped = -input[prev].y > input[prev].w;

    for (i32 i = 0; i < count; ++i)
    {
        bool clipped = -input[i].y > input[i].w;

        if (clipped != prev_clipped)
        {
            output[done] = input[clipped ? i : prev];
            ClipNY(output[done], input[clipped ? prev : i]);
            ++done;
        }

        if (!clipped)
        {
            output[done] = input[i];
            ++done;
        }

        prev_clipped = clipped;
        prev = i;
    }

    return done;
}

// ?ClipPY@@YAHPAUCV@@0H@Z
static i32 ClipPY(CV* output, CV* input, i32 count)
{
    i32 done = 0;
    i32 prev = count - 1;
    bool prev_clipped = input[prev].y > input[prev].w;

    for (i32 i = 0; i < count; ++i)
    {
        bool clipped = input[i].y > input[i].w;

        if (clipped != prev_clipped)
        {
            output[done] = input[clipped ? i : prev];
            ClipPY(output[done], input[clipped ? prev : i]);
            ++done;
        }

        if (!clipped)
        {
            output[done] = input[i];
            ++done;
        }

        prev_clipped = clipped;
        prev = i;
    }

    return done;
}

// ?ClipNZ@@YAHPAUCV@@0H@Z
static i32 ClipNZ(CV* output, CV* input, i32 count)
{
    i32 done = 0;
    i32 prev = count - 1;
    bool prev_clipped = -input[prev].z > input[prev].w;

    for (i32 i = 0; i < count; ++i)
    {
        bool clipped = -input[i].z > input[i].w;

        if (clipped != prev_clipped)
        {
            output[done] = input[clipped ? i : prev];
            ClipNZ(output[done], input[clipped ? prev : i]);
            ++done;
        }

        if (!clipped)
        {
            output[done] = input[i];
            ++done;
        }

        prev_clipped = clipped;
        prev = i;
    }

    return done;
}

// ?ClipPZ@@YAHPAUCV@@0H@Z
static i32 ClipPZ(CV* output, CV* input, i32 count)
{
    i32 done = 0;
    i32 prev = count - 1;
    bool prev_clipped = input[prev].z > input[prev].w;

    for (i32 i = 0; i < count; ++i)
    {
        bool clipped = input[i].z > input[i].w;

        if (clipped != prev_clipped)
        {
            output[done] = input[clipped ? i : prev];
            ClipPZ(output[done], input[clipped ? prev : i]);
            ++done;
        }

        if (!clipped)
        {
            output[done] = input[i];
            ++done;
        }

        prev_clipped = clipped;
        prev = i;
    }

    return done;
}

// ?FullClip@@YAHPAUCV@@0H@Z
static i32 FullClip(CV* ARTS_RESTRICT output, CV* ARTS_RESTRICT input, i32 count)
{
    if (count = ClipNZ(output, input, count); count == 0)
        return 0;

    if (count = ClipNX(input, output, count); count == 0)
        return 0;

    if (count = ClipPX(output, input, count); count == 0)
        return 0;

    if (count = ClipNY(input, output, count); count == 0)
        return 0;

    if (count = ClipPY(output, input, count); count == 0)
        return 0;

    if (count = ClipPZ(input, output, count); count == 0)
        return 0;

    for (i32 i = 0; i < count; ++i)
        output[i] = input[i];

    return count;
}

// ?ZClipOnly@@YAHPAUCV@@0H@Z
static i32 ZClipOnly(CV* ARTS_RESTRICT output, CV* ARTS_RESTRICT input, i32 count)
{
    if (count = ClipNZ(output, input, count); count == 0)
        return 0;

    if (count = ClipPZ(input, output, count); count == 0)
        return 0;

    // NOTE: Original code did not copy the verts
    // Without the copy, there are artifacts, since the last clip wrote to the input
    for (i32 i = 0; i < count; ++i)
        output[i] = input[i];

    return count;
}

static inline u8 CalculateFog(f32 w, f32 fog)
{
    return ~FloatToByte(std::min<f32>(w * fog, 255.0f));
}

#define ARTS_TRANSFORM_FOG fogout[i] = CalculateFog(output[i].w, FogValue)

#define ARTS_TRANSFORM_CODE                                                                    \
    f32 w_abs = std::abs(output[i].w);                                                         \
    u8 clip_code = ((((w_abs - std::abs(output[i].x)) < 0.0f) << ((output[i].x < 0.0f) + 0)) | \
        (((w_abs - std::abs(output[i].y)) < 0.0f) << ((output[i].y < 0.0f) + 2)) |             \
        (((w_abs - std::abs(output[i].z)) < 0.0f) << ((output[i].z < 0.0f) + 4)));             \
    clip_any |= clip_code;                                                                     \
    clip_all &= clip_code;                                                                     \
    out_codes[i] = clip_code;

#ifndef ARTS_ENABLE_KNI
void agiMeshSet::ToScreen(u8* ARTS_RESTRICT in_codes, Vector4* ARTS_RESTRICT verts, i32 count)
{
    ARTS_UTIMED(agiInvertTimer);

    for (i32 i = 0; i < count; ++i)
    {
        if (!(in_codes[i] & MESH_CLIP_SCREEN))
            continue;

        Vector4& vert = verts[i];

        f32 const inv_w = 1.0f / vert.w;

        vert.x = (vert.x * inv_w * HalfWidth) + OffsX;
        vert.y = (vert.y * inv_w * HalfHeight) + OffsY;
        vert.z = (vert.z * inv_w * DepthScale) + DepthOffset;
        vert.w = inv_w;

        ClampToScreen(vert);
    }
}

#    define ARTS_TRANSFORM_DOT                                                                  \
        output[i].x = M.m0.x * input[i].x + M.m1.x * input[i].y + M.m2.x * input[i].z + M.m3.x; \
        output[i].y = M.m0.y * input[i].x + M.m1.y * input[i].y + M.m2.y * input[i].z + M.m3.y; \
        output[i].w = M.m0.z * input[i].x + M.m1.z * input[i].y + M.m2.z * input[i].z + M.m3.z; \
        output[i].z = output[i].w * ProjZZ + ProjZW

void agiMeshSet::Transform(Vector4* ARTS_RESTRICT output, Vector3* ARTS_RESTRICT input, i32 count)
{
    STATS.VertsXfrm += count;

    if (FogValue == 0.0f)
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_TRANSFORM_DOT;
        }
    }
    else
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_TRANSFORM_DOT;
            ARTS_TRANSFORM_FOG;
        }
    }
}

u32 agiMeshSet::TransformOutcode(
    u8* ARTS_RESTRICT out_codes, Vector4* ARTS_RESTRICT output, Vector3* ARTS_RESTRICT input, i32 count)
{
    STATS.VertsOut += count;
    STATS.VertsXfrm += count;

    u8 clip_any = 0;
    u8 clip_all = 0xFF;

    if (FogValue == 0.0f)
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_TRANSFORM_DOT;
            ARTS_TRANSFORM_CODE;
        }
    }
    else
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_TRANSFORM_DOT;
            ARTS_TRANSFORM_FOG;
            ARTS_TRANSFORM_CODE;
        }
    }

    return clip_any | (clip_all << 8);
}

void agiBlendColors(u32* ARTS_RESTRICT shaded, u32* ARTS_RESTRICT colors, i32 count, u32 color)
{
    if (count)
    {
        const u32 mul_b = (color & 0xFF) * 0x8081;
        const u32 mul_g = ((color >> 8) & 0xFF) * 0x8081;
        const u32 mul_r = ((color >> 16) & 0xFF) * 0x8081;
        const u32 mul_a = (color >> 24) * 0x8081;

        for (i32 i = 0; i < count; ++i)
        {
            const u32 input = colors[i];

            const u8 b = static_cast<u8>((mul_b * (input & 0xFF)) >> 23);
            const u8 g = static_cast<u8>((mul_g * ((input >> 8) & 0xFF)) >> 23);
            const u8 r = static_cast<u8>((mul_r * ((input >> 16) & 0xFF)) >> 23);
            const u8 a = static_cast<u8>((mul_a * (input >> 24)) >> 23);

            shaded[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}
#endif

#define ARTS_SHADOW_TRANSFORM_DOT                                                                                    \
    output[i].x = ShadowMatrix.m0.x * input[i].x + ShadowMatrix.m1.x * input[i].y + ShadowMatrix.m2.x * input[i].z + \
        ShadowMatrix.m3.x;                                                                                           \
    output[i].y = ShadowMatrix.m0.y * input[i].x + ShadowMatrix.m1.y * input[i].y + ShadowMatrix.m2.y * input[i].z + \
        ShadowMatrix.m3.y;                                                                                           \
    output[i].z = ShadowMatrix.m0.z * input[i].x + ShadowMatrix.m1.z * input[i].y + ShadowMatrix.m2.z * input[i].z + \
        ShadowMatrix.m3.z;                                                                                           \
    output[i].w = ShadowMatrix.m0.w * input[i].x + ShadowMatrix.m1.w * input[i].y + ShadowMatrix.m2.w * input[i].z + \
        ShadowMatrix.m3.w;

void agiMeshSet::ShadowTransform(Vector4* ARTS_RESTRICT output, Vector3* ARTS_RESTRICT input, i32 count)
{
    STATS.VertsXfrm += count;

    if (FogValue == 0.0f)
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_SHADOW_TRANSFORM_DOT;
        }
    }
    else
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_SHADOW_TRANSFORM_DOT;
            ARTS_TRANSFORM_FOG;
        }
    }
}

u32 agiMeshSet::ShadowTransformOutcode(
    u8* ARTS_RESTRICT out_codes, Vector4* ARTS_RESTRICT output, Vector3* ARTS_RESTRICT input, i32 count)
{
    STATS.VertsOut += count;
    STATS.VertsXfrm += count;

    u8 clip_any = 0;
    u8 clip_all = 0xFF;

    if (FogValue == 0.0f)
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_SHADOW_TRANSFORM_DOT;
            ARTS_TRANSFORM_CODE;
        }
    }
    else
    {
        for (i32 i = 0; i < count; ++i)
        {
            ARTS_SHADOW_TRANSFORM_DOT;
            ARTS_TRANSFORM_FOG;
            ARTS_TRANSFORM_CODE;
        }
    }

    return clip_any | (clip_all << 8);
}

void agiMeshSet::SetFog(f32 fog, i32 /*arg2*/)
{
    FogValue = fog ? (255.0f / fog) : 0.0f;
}

// TODO: Process all clipped CV's at once, similar to ToScreen
void agiMeshSet::ClipTri(i32 i1, i32 i2, i32 i3, i32 texture)
{
    ARTS_UTIMED(agiClipTimer);

    if ((ClippedVertCount + 16) > ARTS_SIZE(ClippedVerts) || ClippedTriCount == ARTS_SIZE(ClippedTris))
        Quitf("ClipTri: clip buffer overflow");

    PodArray<CV, 16> cv_in;
    cv_in[0] = {out[VertexIndices[i1]], 1.0f, 0.0f, 0.0f};
    cv_in[1] = {out[VertexIndices[i2]], 0.0f, 1.0f, 0.0f};
    cv_in[2] = {out[VertexIndices[i3]], 0.0f, 0.0f, 1.0f};

    u32 count = OnlyZClip ? ZClipOnly(&ClippedVerts[ClippedVertCount], cv_in, 3)
                          : FullClip(&ClippedVerts[ClippedVertCount], cv_in, 3);

    if (count)
    {
        for (u32 i = 0; i < count; ++i)
        {
            CV& vert = ClippedVerts[ClippedVertCount + i];

            f32 inv_w = 1.0f / vert.w;
            f32 x = vert.x * inv_w * HalfWidth + OffsX;
            f32 y = vert.y * inv_w * HalfHeight + OffsY;
            f32 z = vert.z * inv_w * DepthScale + DepthOffset;

            vert.fog = CalculateFog(vert.w, FogValue);
            vert.x = x;
            vert.y = y;
            vert.z = z;
            vert.w = inv_w;

            ClampToScreen(vert);
        }

        ClippedTris[ClippedTriCount] = {ClippedVertCount, count,
            {static_cast<u32>(i1), static_cast<u32>(i2), static_cast<u32>(i3)}, ClippedTextures[texture]};
        ClippedTextures[texture] = &ClippedTris[ClippedTriCount];
        ++ClippedTriCount;
        ClippedVertCount += count;
    }
}

void agiMeshSet::InitViewport(agiViewParameters& params)
{
    f32 pipe_width = static_cast<f32>(Pipe()->GetWidth());
    f32 pipe_height = static_cast<f32>(Pipe()->GetHeight());

    f32 x = std::round(pipe_width * params.X);
    f32 y = std::round(pipe_height * params.Y);
    f32 w = std::round(pipe_width * (params.X + params.Width)) - x;
    f32 h = std::round(pipe_height * (params.Y + params.Height)) - y;
    y = pipe_height - (y + h);

    HalfWidth = w * 0.5f;
    HalfHeight = -h * 0.5f;

    OffsX = x + HalfWidth;
    OffsY = y - HalfHeight;

    if (GetRendererInfo().SpecialFlags & 0x8)
        HalfHeight *= 1.01f;

    MinX = OffsX - HalfWidth;
    MaxX = OffsX + HalfWidth;
    MinY = OffsY + HalfHeight;
    MaxY = OffsY - HalfHeight;

    OnlyZClip = false;
    ClipMask = MESH_CLIP_ANY;

    if (GetRendererInfo().SpecialFlags & 0x20)
    {
        // TODO: Use viewport scissor region
        // TODO: Allow custom [Min/Max]Z
        if (MinX <= 0.0f && MaxX >= pipe_width && MinY <= 0.0f && MaxY >= pipe_height)
        {
            constexpr float HugeVal = 1e10f;

            MinX = -HugeVal;
            MaxX = +HugeVal;
            MinY = -HugeVal;
            MaxY = +HugeVal;

            OnlyZClip = true;
            ClipMask = MESH_CLIP_NZ | MESH_CLIP_PZ;
        }
    }
    else
    {
        MinX = std::max(MinX, x);
        MaxX = std::min(MaxX, x + w);
        MinY = std::max(MinY, y);
        MaxY = std::min(MaxY, y + h);

        MinX = std::max(MinX, 0.0f);
        MaxX = std::min(MaxX, pipe_width);
        MinY = std::max(MinY, 0.0f);
        MaxY = std::min(MaxY, pipe_height);
    }

    if (FlipX)
        HalfWidth = -HalfWidth;
}

// Which draw entry points are allowed to take the hardware-transform path, as a bitmask from the
// OPEN1560_NATIVE_MASK environment variable (default: all). Exists so a rendering regression can be
// bisected against a running game in seconds instead of one seven-minute rebuild per hypothesis -
// set it to 0 to get the original CPU-pretransform behavior back without rebuilding at all.
#define NATIVE_DRAW 0x1u
#define NATIVE_DRAWLIT 0x2u
#define NATIVE_DRAWLITENV 0x4u
#define NATIVE_DRAWCOLOR 0x8u
#define NATIVE_DRAWCARD 0x20u
#define NATIVE_DRAWLINES 0x40u

// Default rises to 0x7F with the model path (pedestrians, agiworld/meshmodel.cpp) at 0x10 and the
// billboard and line paths (agiworld/cardworld.h) at 0x20 and 0x40.
bool agiNativePathEnabled(u32 which);

static bool NativePathEnabled(u32 which)
{
    return agiNativePathEnabled(which);
}

bool agiNativePathEnabled(u32 which)
{
    static const u32 mask = []() -> u32 {
    // std::getenv is deprecated by MSVC and this build is /WX; the alternatives are either
    // windows.h in agiworld (invasive) or getenv_s (Annex K). Reading one environment variable
    // once at startup is not the unsafety the deprecation is aimed at, so suppress it locally.
#pragma warning(push)
#pragma warning(disable : 4996)
        const char* value = std::getenv("OPEN1560_NATIVE_MASK");
#pragma warning(pop)

        // 0x7F. 0x1F was the previous default: the four agiMeshSet entry points plus
        // NATIVE_DRAWMODEL (0x10) for pedestrians, which are submitted through DrawNativeTransform
        // where before they were CPU-pretransformed and reacted to nothing. 0x20 and 0x40 add the
        // billboard and line paths, which are the whole effects layer - smoke, spray, debris,
        // coronas, headlight and tail-light flares, collision sparks. Those were the last 3D
        // content still going out pretransformed, i.e. the last thing missing from a Remix capture.
        u32 parsed = value ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0x7Fu;
        Displayf("DX9 NATIVE_MASK = 0x%X (%s)", parsed, value ? "from environment" : "default");
        return parsed;
    }();

    return (mask & which) != 0;
}

b32 agiMeshSet::Draw(u32 flags)
{
    // FIXME: Avoid this check
    // Lots of unchecked calls in aiVehicleInstance::Draw
    if (agiMeshSet* volatile this_ptr = this; !this_ptr)
        return false;

    bool drawn = false;

    if (LockIfResident())
    {
        // Deliberately not gated on Normals: DrawNativeTransform() submits normal-less meshes
        // unlit, exactly as FirstPass() would have. Requiring normals here was what kept most
        // static city scenery and every low-detail LOD on the CPU pretransform path - measured at
        // ~30% of scene triangles, and growing with scene density.
        if (Pipe()->SupportsNativeTransform() && NativePathEnabled(NATIVE_DRAW))
        {
            // Unlit, unconditionally. Draw() is the *unlit* entry point: its CPU branch below is
            // FirstPass(Colors, TexCoords, 0xFFFFFFFF), which runs no lighter at all and hands the
            // mesh's baked per-adjunct colours straight to the rasterizer. Leaving `unlit` at its
            // default let every mesh that merely *has* normals get hardware-lit here instead, by
            // whichever rig static_lighting selected - and with static_lighting false that is the
            // real-time dynamic list (agiLighter::LIGHTS, headlights and coronas), which carries
            // essentially nothing for static scenery. This is the path DrawLit() falls back to when
            // the lighter is null, i.e. all of AGI_QUALITY_LOW (mmcity/cullcity.cpp, fix_lighting
            // sets both StaticLighter and DynamicLighter to nullptr there), and it is also where
            // software-rendering-disabled StaticLighter content lands. The result was city geometry
            // rendered near-black at low light quality while the same content on the CPU path was
            // correctly bright.
            drawn = DrawNativeTransform(flags, false, nullptr, nullptr, /*unlit=*/true);
        }
        else if (Geometry(flags, Vertices, Planes) <= 0xFF)
        {
            FirstPass(Colors, TexCoords, 0xFFFFFFFF);
            drawn = true;
        }

        Unlock();
    }
    else
    {
        PageIn();
    }

    return drawn;
}

b32 agiMeshSet::DrawColor(u32 color, u32 flags)
{
    bool drawn = false;

    if (LockIfResident())
    {
        if (Pipe()->SupportsNativeTransform() && NativePathEnabled(NATIVE_DRAWCOLOR))
        {
            // Same colour resolution FirstPass() performs, done up front so the result can be
            // handed to the hardware path as final per-vertex colours: modulate the mesh's own
            // colours by `color`, or - when the mesh has none - use `color` for every vertex.
            // These draws are unlit by definition (the caller is dictating the colour), so
            // hardware lighting is forced off even for meshes that do carry normals.
            const u32* final_colors = Colors;

            if (color != 0xFFFFFFFF)
            {
                u32* shaded = ARTS_ALLOCA(u32, AdjunctCount);

                if (Colors)
                {
                    ARTS_UTIMED(agiLightTimer);
                    agiBlendColors(shaded, Colors, AdjunctCount, color);
                }
                else
                {
                    for (u32 i = 0; i < AdjunctCount; ++i)
                        shaded[i] = color;
                }

                final_colors = shaded;
            }

            drawn = DrawNativeTransform(flags, false, nullptr, final_colors, /*unlit=*/true);
        }
        else if (Geometry(flags, Vertices, Planes) <= 0xFF)
        {
            u32* colors = Colors;

            if (colors && color != 0xFFFFFFFF)
            {
                u32* shaded = ARTS_ALLOCA(u32, AdjunctCount);

                {
                    ARTS_UTIMED(agiLightTimer);
                    agiBlendColors(shaded, colors, AdjunctCount, color);
                }

                colors = shaded;
                color = 0xFFFFFFFF;
            }

            FirstPass(colors, TexCoords, color);

            drawn = true;
        }

        Unlock();
    }
    else
    {
        PageIn();
    }

    return drawn;
}

// True for the fixed sun/fill1/fill2 + ambient rig the city's static geometry is lit with, as
// opposed to the real-time dynamic light list used for cars and other movers. These are the only
// three lighters mmInstance::StaticLighter/DynamicLighter are ever set to (mmcity/cullcity.cpp),
// but comparing against the agiworld-level functions themselves keeps this layer independent of
// mmcity. Selects agiRasterizer::MeshWorld()'s static_lighting argument - see agi/rsys.h.
static inline bool IsStaticCityLighter(agiMeshLighter lighter)
{
    return (lighter == agiMeshLighterTriple) || (lighter == agiMeshLighterQuarter);
}

// agiConeLighter is deliberately NOT in the list above, and is deliberately kept off the hardware
// path entirely. It is a genuinely different lighting model - selected only by the `-conelighter`
// debug switch (mmcity/cullcity.cpp, fix_lighting), still closed assembly, and named for cone
// geometry rather than the sun/fill/fill rig. Mapping it onto SetupD3D9StaticLights()'s three
// directionals, as this used to, silently substituted a completely different result for whatever it
// actually computes. There is no evidence for that mapping, so the honest behaviour is to let it
// keep running on the CPU where it is correct.
static inline bool CanNativeLight(agiMeshLighter lighter)
{
    return lighter != agiConeLighter;
}

// Vehicle chrome strength, and the two embellishment terms the original SphereMap pass does not
// have. cmd_params because chrome is the kind of thing that is judged by eye, and a rebuild of this
// project is a CI round trip.
//
// 0.35 rather than the original's literal full strength: SphereMap composited over a body the CPU
// had lit, and this path keeps a hardware-lit base pass underneath, so an additive layer at full
// alpha reads hotter here than it did there. Set -reflectamount 1 for the literal original.
static mem::cmd_param PARAM_reflect_amount {"reflectamount", "Vehicle sphere-map reflection strength"};
static mem::cmd_param PARAM_reflect_fresnel_bias {"reflectfresnelbias", "Vehicle reflection fresnel bias"};
static mem::cmd_param PARAM_reflect_fresnel_scale {"reflectfresnelscale", "Vehicle reflection fresnel scale"};
static mem::cmd_param PARAM_reflect_specular {"reflectspecular", "Vehicle reflection sun-glint strength"};

// See the note in DrawLit. Traffic and opponent cars never reach DrawLitSph, so without this they
// get no chrome at all - but this layer cannot tell a car from a building, so switching it on
// chromes both. Opt-in until there is a hook that can see the caller.
static mem::cmd_param PARAM_traffic_reflect {"trafficrefl", "Sphere-map reflections on AI and traffic vehicles"};

// The city's shared vehicle sphere map, published once per frame by mmCullCity::Cull().
//
// Both vehicle draw paths read the same texture out of mmCullCity - mmCarModel::Draw passes it to
// DrawLitSph, aiVehicleInstance::Draw passes it to SphereMap - but agiworld deliberately does not
// depend on mmcity, so mmcity pushes it down here instead of this reaching up for it.
agiTexDef* agiNativeCitySphereMap = nullptr;

u32 agiReflectDraws = 0;
u32 agiReflectSkipNoNormals = 0;

void agiResetReflectStats()
{
    agiReflectDraws = 0;
    agiReflectSkipNoNormals = 0;
}

b32 agiMeshSet::DrawLit(agiMeshLighter lighter, u32 flags, u32* colors)
{
    // No lighter: the LOW lighting setting, where fix_lighting (mmcity/cullcity.cpp) clears both
    // mmInstance lighters. The original forwards straight to Draw(flags) here, and Draw() colours
    // the mesh with its OWN Colors - so `colors` is thrown away. For most callers that loses nothing,
    // because they pass null. Traffic does not: aiVehicleInstance::Draw hands each car's paint in
    // as `colors` (its per-LOD colour table, game.asm ~98700), over a body mesh whose own colours
    // are a neutral grey. So at LOW every randomly painted car came out grey.
    //
    // Unlit is still what LOW asks for, so this draws exactly what Draw() would, with the caller's
    // colours in place of the mesh's.
    if (!lighter)
    {
        if (!colors)
            return Draw(flags);

        bool drawn = false;

        if (LockIfResident())
        {
            if (Pipe()->SupportsNativeTransform() && NativePathEnabled(NATIVE_DRAW))
            {
                drawn = DrawNativeTransform(flags, false, nullptr, colors, /*unlit=*/true);

                // Same contract as the lit hardware branch below: aiVehicleInstance::Draw follows a
                // successful DrawLit with SphereMap(), which reads CPU scratch this path never
                // writes. Report "not drawn" so it skips the overlay - see the note there.
                if (drawn && agiRQ.SphMap)
                    drawn = false;
            }
            else if (Geometry(flags, Vertices, Planes) <= 0xFF)
            {
                FirstPass(colors, TexCoords, 0xFFFFFFFF);
                drawn = true;
            }

            Unlock();
        }
        else
        {
            PageIn();
        }

        return drawn;
    }

    // City geometry (building facades, ground/road, animated set pieces) reaches the renderer
    // through here, and it is the bulk of the scene - so this is the branch that decides whether
    // RTX Remix sees a real 3D world or a stream of screen-space triangles. Route it through the
    // hardware-transform path, which submits model-space vertices plus a real SetTransform(WORLD),
    // and let the GPU do the lighting the `lighter` callback would otherwise have baked into
    // per-vertex colors on the CPU.
    //
    // Three previous attempts at this were reverted, all reporting city geometry that vanished or
    // rendered "detached/floating". That is now explained: BuildProjectionMatrix() had an inverted
    // sign on the projection's Z row (see dx9rsys.cpp), which drove clip-space Z negative for every
    // vertex, so *all* native-transform geometry failed D3D9's near-plane test and was discarded
    // while the draw still reported success. Those reverts were diagnosing the projection bug, not
    // a World-matrix bug: the "World is (0,0,0)" observation behind the last one only ever printed
    // World.m3, and a zero translation is simply an identity world transform - correct, and
    // rendered correctly here, for cell geometry whose vertices are already world-space.
    //
    // `colors` overrides the mesh's own per-adjunct base colours; it is forwarded as the hardware
    // path's base colours, which is the same role it plays on the CPU path (there it is handed to
    // the lighter as the base to shade). Requires normals - this branch asks the GPU to light, and
    // agiMeshLighter* would equally fault on mesh->Normals[i] without them, so a normal-less mesh
    // never legitimately reaches DrawLit() with a lighter in the first place.
    if (Pipe()->SupportsNativeTransform() && Normals && CanNativeLight(lighter) && NativePathEnabled(NATIVE_DRAWLIT))
    {
        // Same residency handling Draw() wraps around DrawNativeTransform(): it reads
        // Vertices/Normals/Colors/TexCoords directly, so the mesh has to be locked in memory for
        // the duration, and a non-resident mesh needs a PageIn() request rather than a silent
        // no-op that never asks for the data and so never becomes drawable.
        bool native_drawn = false;

        // Chrome for the vehicles that never reach DrawLitSph.
        //
        // There are two vehicle draw paths and they do not agree. mmCarModel::Draw (the player, and
        // the showroom via mmVehicleForm::Cull) calls DrawLitSph, which sets the two globals below
        // for us. aiVehicleInstance::Draw - traffic, opponents, police, every other car in the city
        // - does not: it calls DrawLit and then SphereMap directly (game.asm:98860), and we suppress
        // that SphereMap call just below. So wiring the replacement to DrawLitSph alone covered the
        // player's car and left every other car on the road matte.
        //
        // DEFAULT OFF, and the reason is a correction to an earlier attempt at this.
        //
        // That attempt gated on `!IsStaticCityLighter(lighter)`, reasoning that vehicles are lit by
        // the dynamic rig and city geometry by the static one. They are not different rigs.
        // fix_lighting (mmcity/cullcity.cpp) assigns mmInstance::StaticLighter AND
        // mmInstance::DynamicLighter from the same pair of functions - both are agiMeshLighterTriple
        // or agiMeshLighterQuarter at every light quality - so IsStaticCityLighter() is true for a
        // car body exactly as it is for a building facade. The gate could never pass, and the
        // traffic chrome it was supposed to enable never ran once.
        //
        // With that condition gone there is nothing left at this layer that separates a vehicle from
        // any other lit mesh drawn through DrawLit: the assembly distinguishes them by the `lod == 3`
        // test at its own call site (game.asm:98860), which is not visible from here. So turning
        // this on puts sphere-map chrome on building facades too. That is why it is opt-in now
        // rather than on by default - a wrong default here is a whole-city visual regression.
        //
        // Doing this properly needs a hook that can see the caller, not a better guess at this
        // layer. Reimplementing agiMeshSet::SphereMap is the obvious candidate: the assembly calls
        // it with the right mesh at the right moment, and it is the one place that knows.
        const bool traffic_reflect =
            PARAM_traffic_reflect.get_or(false) && agiRQ.SphMap && !agiNativeReflectionTex && agiNativeCitySphereMap;

        const f32 saved_reflectivity = agiNativeReflectivity;
        agiTexDef* const saved_reflection_tex = agiNativeReflectionTex;

        if (traffic_reflect)
        {
            agiNativeReflectivity = 1.0f;
            agiNativeReflectionTex = agiNativeCitySphereMap;
        }

        if (LockIfResident())
        {
            native_drawn = DrawNativeTransform(flags, IsStaticCityLighter(lighter), nullptr, colors);

            Unlock();
        }
        else
        {
            PageIn();
        }

        agiNativeReflectivity = saved_reflectivity;
        agiNativeReflectionTex = saved_reflection_tex;

        // DrawLit() carries an implicit contract with closed ARTS_IMPORT callers that run a second
        // pass over the same mesh afterwards: it leaves the CPU Geometry() scratch state
        // (codes/out/firstFacet/nextFacet/vertCounts/indexCounts) describing what was just drawn,
        // and they read it instead of recomputing. aiVehicleInstance::Draw (game.asm:98859) is the
        // one such caller left - it calls DrawLit() and then, for the highest LOD, SphereMap() for
        // the vehicle reflection overlay. DrawNativeTransform() never writes that scratch state, so
        // on the hardware path SphereMap() reads whatever an earlier mesh left behind and aborts
        // with "FATAL ERROR: Bug?".
        //
        // Re-running Geometry() here to repopulate it was tried, and does not work. Geometry()
        // returns a clip code and may legitimately reject the mesh outright - > 0xFF, the very
        // condition the CPU branch below gates FirstPass() on - which leaves the facet chain empty.
        // SphereMap() does not check: it walks firstFacet/nextFacet accumulating a count and
        // asserts when that count is zero (game.asm:334168, the third 'Bug?' site). So the assert
        // still fired, and every vehicle paid for a full CPU transform per frame to achieve it.
        //
        // Report "not drawn" instead, which makes the `test eax,eax / jz` at game.asm:98860 skip
        // the overlay. That is safe because this is the *only* one of DrawLit()'s call sites that
        // reads the return value at all - the other seventeen discard eax immediately - so nothing
        // else changes behaviour. The body itself was already submitted by DrawNativeTransform()
        // above; only the chrome overlay is dropped, which is exactly the policy DrawLitSph() below
        // already applies deliberately for the same reason.
        //
        // The overlay is a CPU-pretransformed pass and so invisible to RTX Remix anyway, while the
        // vehicle body underneath goes out in world space, which is what Remix needs. With
        // traffic_reflect above, the replacement has already run as part of the draw - so what this
        // suppresses is a duplicate rather than the only copy. With -trafficrefl 0 it is once again
        // dropping the overlay outright, which is the older, matte behaviour.
        if (native_drawn && agiRQ.SphMap)
            return false;

        return native_drawn;
    }

    bool drawn = false;

    if (LockIfResident())
    {
        if (Geometry(flags, Vertices, Planes) <= 0xFF)
        {
            u32* shaded = ARTS_ALLOCA(u32, AdjunctCount);

            {
                ARTS_UTIMED(agiLightTimer);
                lighter(codes, shaded, colors ? colors : Colors, this);
            }

            FirstPass(shaded, TexCoords, 0);
            drawn = true;
        }

        Unlock();
    }
    else
    {
        PageIn();
    }

    return drawn;
}

// Reflectivity of the draw in flight, 0 for ordinary geometry and 1 for a vehicle body.
//
// A global rather than a parameter because agiMeshSet's layout is frozen and DrawLitSph reaches
// MeshWorld through DrawLit and DrawNativeTransform, neither of which can grow an argument without
// touching call sites the assembly owns. agiNativeDrawRadius sets the precedent.
f32 agiNativeReflectivity = 0.0f;

// The game's own authored reflection texture for the vehicle in flight - the sphere map
// DrawLitSph() is handed, which is what the original renderer wrapped around a car body. Preferred
// over the synthesised probe for vehicles: it is real art, it is what the cars were built to look
// like, and it costs nothing to use because the engine already loaded and passed it.
agiTexDef* agiNativeReflectionTex = nullptr;

// See agi/rsys.h. Scoped to the one DrawNativeTransform call in DrawLitEnv.
bool agiNativeGroundDraw = false;

void agiMeshSet::DrawLitSph(agiMeshLighter lighter, agiTexDef* sph_map, u32 flags)
{
    // Vehicle bodies. DrawLit() now takes the hardware-transform path here too, which is what puts
    // cars into the world-space stream RTX Remix can reconstruct. An earlier attempt at this
    // reported bodies rendering completely invisible (only headlight/taillight sprites left) and
    // was reverted; that was the inverted projection Z sign described in DrawLit() above, which
    // clipped every native-transform draw regardless of subject, not something specific to
    // vehicles or to the vehicle-select showroom camera.
    //
    // SphereMap() itself stays off on the native path: it is closed assembly that reads the CPU
    // Geometry() pass's leftover scratch state, which this path never populates, and calling it
    // there hard-crashes.
    //
    // What replaces it is a fixed-function world-space second pass - see BuildVehicleReflectionVertices
    // in agidx9/dx9rsys.cpp, whose UV derivation was decoded from SphereMap's own disassembly rather
    // than taken from a textbook, because the game ships authored sphere maps and reading them at
    // the wrong coordinates gives the wrong car. This is the one entry point vehicle bodies come
    // through - player, traffic and opponents all reach it via aiVehicleInstance::Draw and
    // mmCarModel - so it is where a body is identified as something that should reflect.
    //
    // agiRQ.SphMap keeps its meaning as "vehicle reflections on", which is what the graphics option
    // says it is, and it is the same gate the original applies (SphereMap() opens by testing
    // agiRQ+8, game.asm ~333874).
    const bool reflective = sph_map && agiRQ.SphMap && Pipe()->SupportsNativeTransform();

    agiNativeReflectivity = reflective ? 1.0f : 0.0f;
    agiNativeReflectionTex = reflective ? sph_map : nullptr;

    const b32 drawn = DrawLit(lighter, flags, nullptr);

    // Strictly scoped: anything drawn after this - road, buildings, particles - must not inherit a
    // car's reflectivity or its reflection texture.
    agiNativeReflectivity = 0.0f;
    agiNativeReflectionTex = nullptr;

    if (drawn && sph_map && !Pipe()->SupportsNativeTransform())
        SphereMap(sph_map, 0xFFFFFFFF);
}

void agiMeshSet::DrawLitEnv(agiMeshLighter lighter, agiTexDef* env_map, Matrix34& transform, u32 flags)
{
    if (LockIfResident())
    {
        // Ground/road geometry arrives here. On a multitexturing device it would otherwise take
        // MultiTexEnvMap() below - a fully CPU-pretransformed path, invisible to RTX Remix as 3D
        // geometry - so the hardware-transform path is preferred. Both EnvMap() and
        // MultiTexEnvMap() are closed assembly routines that consume the codes/out/firstFacet
        // scratch the CPU Geometry() pass leaves behind, which DrawNativeTransform() never
        // populates; feeding them stale scratch crashes ("Bug?"), so neither can be called here.
        //
        // The env pass itself is no longer dropped. agiNativeMaterialFx::EnvTexture carries it
        // down to a world-space second pass that reproduces EnvMap()'s projection exactly - see
        // BuildGroundEnvVertices in agidx9/dx9rsys.cpp, which was decoded from the original rather
        // than guessed. That restores the road's baked shadow and sheen, and unlike the routine it
        // replaces it is model-space geometry that RTX Remix can place.
        //
        // The Normals gate stays, unchanged. The env projection itself is planar and needs only the
        // vertex position, so it would work on normal-less terrain too - but removing the gate would
        // also reroute normal-less ground away from the CPU lighting it currently gets, and those
        // meshes carry no vertex colours either (mmInstance::InitMeshes asks for neither), so they
        // would come out flat white. That is a separate change and wants its own evidence.
        if (Pipe()->SupportsNativeTransform() && Normals && CanNativeLight(lighter) &&
            NativePathEnabled(NATIVE_DRAWLITENV))
        {
            // agiRQ.EnvMap is the same gate the original applies - EnvMap() opens by testing it
            // (game.asm ~333257, agiRQ+4) - so honouring it here keeps the graphics option working.
            agiNativeMaterialFx env_fx {};

            if (env_map && agiRQ.EnvMap)
            {
                env_fx.EnvTexture = env_map;
                env_fx.EnvTransform = &transform;
            }

            // `lighter == nullptr` means "do not light this" - the CPU branches below both funnel
            // that case into DrawLit(), which forwards it to Draw() and so to FirstPass() with no
            // lighter. Ask for the same here rather than letting the GPU light road and terrain
            // geometry off the dynamic light list. See the matching note in Draw().
            agiNativeGroundDraw = true;

            DrawNativeTransform(flags, IsStaticCityLighter(lighter), env_fx.EnvTexture ? &env_fx : nullptr, nullptr,
                /*unlit=*/lighter == nullptr);

            agiNativeGroundDraw = false;
        }
        else if (agiCurState.GetMaxTextures() > 1 && agiRQ.EnvMap)
        {
            if (Geometry(flags, Vertices, Planes) <= 0xFF)
            {
                u32* colors = Colors;

                if (lighter)
                {
                    u32* shaded = ARTS_ALLOCA(u32, AdjunctCount);

                    {
                        ARTS_UTIMED(agiLightTimer);
                        lighter(codes, shaded, colors, this);
                    }

                    colors = shaded;
                }

                MultiTexEnvMap(colors, 0xFFFFFFFF, env_map, transform);
            }
        }
        else if (DrawLit(lighter, flags, nullptr) && env_map)
        {
            // Only reachable on the CPU path now (the native-transform branch above returns
            // first), so EnvMap() is guaranteed the codes/out/firstFacet scratch state the
            // Geometry() pass inside DrawLit() just populated - which is exactly what it reads.
            EnvMap(transform, env_map, 0xFFFFFFFF);
        }

        Unlock();
    }
    else
    {
        PageIn();
    }
}

void agiMeshSet::DrawNormals([[maybe_unused]] Vector3& color)
{
    // FIXME: Avoid this check
    if (agiMeshSet* volatile this_ptr = this; !this_ptr)
        return;

    if (LockIfResident())
    {
#ifdef ARTS_DEV_BUILD
        if (Normals)
        {
            DrawBegin(xconst(ViewParams().World));
            ::DrawColor(color);

            for (u32 i = 0; i < AdjunctCount; ++i)
            {
                Vector3 start = Vertices[VertexIndices[i]];
                Vector3 end = start + UnpackNormal[Normals[i]];
                DrawLine(start, end);
            }

            DrawEnd();
        }
#endif

        Unlock();
    }
    else
    {
        PageIn();
    }
}

void agiMeshSet::DrawShadow(u32 flags, const Vector4& plane, const Vector3& light_dir)
{
    if (const Vector3& camera = ViewParams().Camera.m3;
        camera.x * plane.x + camera.y * plane.y + camera.z * plane.z + plane.w < 0.0f)
        return;

    if (LockIfResident())
    {
        if (ShadowGeometry(flags, Vertices, plane, light_dir) <= 0xFF)
        {
            FirstPass(nullptr, nullptr, ShadowColor);
        }

        Unlock();
    }
    else
    {
        PageIn();
    }
}

static void (agiMeshSet::* const FirstPassFunctions[2][2][2][2])(u32* colors, Vector2* tex_coords, u32 color) {
    {
        {
            {&agiMeshSet::FirstPass_HW_UV_CPV_noDYNTEX, &agiMeshSet::FirstPass_HW_UV_CPV_DYNTEX},
            {&agiMeshSet::FirstPass_HW_UV_noCPV_noDYNTEX, &agiMeshSet::FirstPass_HW_UV_noCPV_DYNTEX},
        },
        {
            {&agiMeshSet::FirstPass_HW_noUV_CPV_noDYNTEX, &agiMeshSet::FirstPass_HW_noUV_CPV_DYNTEX},
            {&agiMeshSet::FirstPass_HW_noUV_noCPV_noDYNTEX, &agiMeshSet::FirstPass_HW_noUV_noCPV_DYNTEX},
        },
    },
    {
        {
            {&agiMeshSet::FirstPass_SW_UV_CPV_noDYNTEX, &agiMeshSet::FirstPass_SW_UV_CPV_DYNTEX},
            {&agiMeshSet::FirstPass_SW_UV_noCPV_noDYNTEX, &agiMeshSet::FirstPass_SW_UV_noCPV_DYNTEX},
        },
        {
            {&agiMeshSet::FirstPass_SW_noUV_CPV_noDYNTEX, &agiMeshSet::FirstPass_SW_noUV_CPV_DYNTEX},
            {&agiMeshSet::FirstPass_SW_noUV_noCPV_noDYNTEX, &agiMeshSet::FirstPass_SW_noUV_noCPV_DYNTEX},
        },
    },
};

// ?DynTexFlag@@3HA
ARTS_IMPORT extern i32 DynTexFlag; // mmCarModel::Draw, mmVehicleForm::Cull

void agiMeshSet::FirstPass(u32* colors, Vector2* tex_coords, u32 color)
{
    // FIXME: SW_noUV_* does not unbind the current texture
    if (tex_coords == nullptr)
    {
        agiCurState.SetTexture(nullptr);
    }

    if (agiCurState.GetDrawMode() == agiDrawDepth)
    {
        colors = nullptr;
        color = 0xFF202020;
    }

    ARTS_UTIMED(agiFirstPass);

    (this->*FirstPassFunctions[agiCurState.GetSoftwareRendering()][tex_coords == nullptr][colors == nullptr][(DynTexFlag & MESH_DRAW_DYNTEX) != 0])(
        colors, tex_coords, color);
}

// ?CurrentMeshSetVariant@@3HA
ARTS_IMPORT extern i32 CurrentMeshSetVariant;

template <typename T>
static inline void fill_bytes(T* dst, usize len, u8 value)
{
    std::memset(dst, value, len * sizeof(T));
}

i32 agiMeshSet::Geometry(u32 flags, Vector3* verts, Vector4* planes)
{
    ArAssert(Resident == 2, "Mesh not loaded");

    ClippedVertCount = 0;
    ClippedTriCount = 0;
    fill_bytes(ClippedTextures, TextureCount + 1, 0);

    Init(planes && (SurfaceCount > 1));

    u32 clip_mask = (flags & MESH_DRAW_CLIP) ? MESH_CLIP_ANY : 0; // clip_any | (clip_all << 8)

    {
        ARTS_UTIMED(agiTransformTimer);

        // If available, transform the bounding box, checking how visible it is
        if (clip_mask && BoundingBox)
        {
            clip_mask = TransformOutcode(codes, out, BoundingBox, 8);

            if (clip_mask > 0xFF) // All verts are clipped (not visible)
            {
                // TransformOutcode's per-axis test is |coord| <= |w|, which doesn't check w's
                // sign. A corner behind the camera (w<=0) can still numerically satisfy that test
                // and land on whichever side the sign coincidence favors, so when the camera is
                // close enough to be inside (or straddling) this bounding box - some corners
                // ahead (w>0), some behind (w<=0) - the "all 8 corners agree on an out-of-bounds
                // bit" AND-reduction this early-reject relies on can trigger on a coincidental
                // match between meaningless behind-camera outcodes and genuine in-front ones,
                // wrongly discarding an object that's actually still partially visible (confirmed
                // via diagnostic: large city instance-chain bounding boxes - e.g. a batch of
                // roadside trees - show exactly this mixed-sign pattern at the point this
                // early-reject fires). Only trust the quick reject when every corner agrees on
                // being in front of the camera; otherwise fall through to the slower per-vertex
                // path below, which re-derives clip_mask from the mesh's real geometry instead of
                // its bounding box.
                bool all_in_front = true;

                for (i32 i = 0; i < 8 && all_in_front; ++i)
                {
                    all_in_front = out[i].w > 0.0f;
                }

                if (all_in_front)
                    return clip_mask;

                clip_mask = MESH_CLIP_ANY;
            }
        }

        if (clip_mask)
        {
            clip_mask = TransformOutcode(codes, out, verts, VertexCount);

            if (clip_mask > 0xFF) // All verts are clipped (not visible)
                return clip_mask;
        }
        else // Assume nothing needs clipping
        {
            Transform(out, verts, VertexCount);
        }
    }

    {
        ARTS_UTIMED(agiTraverseTimer);

        fill_bytes(firstFacet, TextureCount + 1, 0xFF);
        fill_bytes(vertCounts, TextureCount + 1, 0);
        fill_bytes(indexCounts, TextureCount + 1, 0);

        DynTexFlag = flags & MESH_DRAW_DYNTEX;
        CurrentMeshSetVariant = std::min<i32>(MESH_DRAW_GET_VARIANT(flags), VariationCount - 1);

        if (clip_mask)
        {
            for (u32 i = 0; i < SurfaceCount; ++i)
            {
                u16 surface[4];
                std::memcpy(surface, &SurfaceIndices[i * 4], sizeof(surface));

                u16 indices[4];
                indices[0] = VertexIndices[surface[0]];
                indices[1] = VertexIndices[surface[1]];
                indices[2] = VertexIndices[surface[2]];

                u8 clip_any = codes[indices[0]] | codes[indices[1]] | codes[indices[2]];
                u8 clip_all = codes[indices[0]] & codes[indices[1]] & codes[indices[2]];

                i16 num_verts;
                i16 num_index;

                if (surface[3])
                {
                    indices[3] = VertexIndices[surface[3]];
                    clip_any |= codes[indices[3]];
                    clip_all &= codes[indices[3]];
                    num_verts = 4;
                    num_index = 6;
                }
                else
                {
                    num_verts = 3;
                    num_index = 3;
                }

                if (!(clip_all & MESH_CLIP_ANY) && (!planes || !IsBackfacing(planes[i])))
                {
                    u8 texture = TextureIndices[i];

                    if (clip_any & ClipMask)
                    {
                        if (surface[3])
                        {
                            ClipTri(surface[1], surface[2], surface[3], texture);
                            ClipTri(surface[1], surface[3], surface[0], texture);
                        }
                        else
                        {
                            ClipTri(surface[0], surface[1], surface[2], texture);
                        }
                    }
                    else
                    {
                        vertCounts[texture] += num_verts;
                        indexCounts[texture] += num_index;
                        nextFacet[i] = firstFacet[texture];
                        firstFacet[texture] = static_cast<i16>(i);
                    }

                    codes[indices[0]] |= MESH_CLIP_SCREEN;
                    codes[indices[1]] |= MESH_CLIP_SCREEN;
                    codes[indices[2]] |= MESH_CLIP_SCREEN;

                    if (surface[3])
                        codes[indices[3]] |= MESH_CLIP_SCREEN;
                }
            }
        }
        else
        {
            // Either we didn't use TransformOutcode, or we don't care about the clipping it returned. Either way, initialize codes
            fill_bytes(codes, VertexCount,
#ifdef CLIP_ALL_TO_SCREEN
                MESH_CLIP_SCREEN
#else
                0
#endif
            );

            for (u32 i = 0; i < SurfaceCount; ++i)
            {
                if (!planes || !IsBackfacing(planes[i]))
                {
                    u8 texture = TextureIndices[i];
                    const u16* ARTS_RESTRICT surface = &SurfaceIndices[i * 4];

                    if (surface[3])
                    {
                        vertCounts[texture] += 4;
                        indexCounts[texture] += 6;

#ifndef CLIP_ALL_TO_SCREEN
                        codes[VertexIndices[surface[3]]] |= MESH_CLIP_SCREEN;
#endif
                    }
                    else
                    {
                        vertCounts[texture] += 3;
                        indexCounts[texture] += 3;
                    }

#ifndef CLIP_ALL_TO_SCREEN
                    codes[VertexIndices[surface[0]]] |= MESH_CLIP_SCREEN;
                    codes[VertexIndices[surface[1]]] |= MESH_CLIP_SCREEN;
                    codes[VertexIndices[surface[2]]] |= MESH_CLIP_SCREEN;
#endif

                    nextFacet[i] = firstFacet[texture];
                    firstFacet[texture] = static_cast<i16>(i);
                }
            }
        }
    }

    ToScreen(codes, out, VertexCount);

    return clip_mask;
}

// See meshset.h - additive path used only by renderers with native transform/lighting support
// (agiPipeline::SupportsNativeTransform). Mirrors the facet traversal in Geometry() above, but
// skips Transform/TransformOutcode/ToScreen entirely: individual triangles are clipped by the
// GPU, so there's no need for the CPU-side clip machinery (ClipTri/FullClip/ClippedVerts) either.

// Reconstructs smooth per-vertex normals for the native path.
//
// agiMeshSet stores normals as a u8 index into UnpackNormal[198] - 198 directions over the whole
// sphere, roughly 26 degrees apart. That quantisation is coarse enough to collapse the normals of
// neighbouring vertices onto the *same* table entry, so all three corners of a facet routinely end
// up identical. Interpolating a constant gives a constant, which means the pixel shader is handed a
// flat normal per facet and its output is indistinguishable from flat shading no matter how
// per-pixel the lighting maths is. On low-poly bodywork - which is all of MM1's vehicles - that
// reads exactly like vertex shading, because geometrically it is.
//
// Re-averaging in float across adjuncts that share a vertex recovers the gradient the quantiser
// destroyed. Adjuncts are per-facet-corner, so several of them reference one spatial vertex via
// VertexIndices; summing their normals there and renormalising is the standard smoothing pass.
//
// The angle threshold preserves genuine hard edges. Without it a box would be smoothed into a
// blob: at a cube corner the average points along the diagonal, which is 54.7 degrees off each face
// normal, so a cos threshold of 0.7 (about 45 degrees) keeps the faces flat while still smoothing
// the shallow angles that make up a curved panel.
static mem::cmd_param PARAM_smooth_normals {"smoothnormals", "Rebuild smooth vertex normals for the hardware path"};

// -flatnormals. See the long note at its use in DrawNativeTransform.
static mem::cmd_param PARAM_flat_normals {"flatnormals", "Shade from facet geometry, ignoring stored vertex normals"};

// -nocull. Every form of culling this codebase can reach, off at once.
//
// For an RTX Remix capture the useful frame is the one containing the most geometry, not the one
// that renders fastest: anything culled is absent from the capture and cannot be replaced, and a
// path tracer wants closed shells and far geometry that a rasteriser is happy to throw away. This
// switch is deliberately expensive and is not a play setting.
//
// What it reaches, and what it does not:
//   * backface culling      - D3DCULL_NONE on both submission paths (agidx9/dx9rsys.cpp)
//   * LOD switching         - mmInstance::LodTable and StaticTerrainLodTable pushed out of range,
//                             so everything draws at its highest detail level
//   * distance culling      - ObjectMaxDist, BuildingMaxDist, LightDistances, agiRQ.FarClip
//   * portal/cell visibility - NOT reachable. asRenderWeb::Cull and mmCellRenderer::Cull are both
//                             ARTS_IMPORT (closed assembly), so the room-to-room traversal that
//                             decides which cells are considered at all cannot be disabled from
//                             here. Everything the traversal does hand down is now drawn.
//
// Queried through a function rather than the parameter directly because mmcity and agidx9 both need
// it and neither may own it - a second cmd_param with the same name would register twice.
static mem::cmd_param PARAM_no_cull {"nocull", "Disable backface, LOD and distance culling (for RTX Remix captures)"};

bool agiNoCullEnabled()
{
    // Resolved once. cmd_param::init() has walked argv long before any draw, and re-reading it per
    // facet would put a string lookup in the inner loop.
    static const bool enabled = PARAM_no_cull.get_or(false);

    return enabled;
}

// -nativecpucull. Restores the camera-dependent CPU backface cull that DrawNativeTransform used to
// apply. Off by default, because it is the single largest source of RTX Remix geometry-hash churn -
// see the long note at the facet-order build in DrawNativeTransform for what it was doing and why
// dropping it costs nothing. Kept as a switch so the two can still be compared directly.
static mem::cmd_param PARAM_native_cpu_cull {
    "nativecpucull", "Cull backfacing facets on the CPU in the world path (breaks RTX Remix hash stability)"};

// Flat shading emits up to four unshared vertices per facet instead of one per adjunct, and those
// live on the stack alongside everything else DrawNativeTransform allocates there. 8192 vertices is
// ~288 KB, which is affordable against the 1 MB stack; a mesh dense enough to exceed it keeps its
// stored normals rather than risking an overflow deep in the draw chain.
inline constexpr u32 kFlatVertexCap = 8192;

u32 agiMeshNormalDraws = 0;
u32 agiMeshNormalDrawsFlat = 0;
u32 agiMeshNormalTris = 0;
u32 agiMeshNormalTrisFlat = 0;

void agiResetMeshNormalStats()
{
    agiMeshNormalDraws = 0;
    agiMeshNormalDrawsFlat = 0;
    agiMeshNormalTris = 0;
    agiMeshNormalTrisFlat = 0;
}

static void SmoothAdjunctNormals(Vector3* ARTS_RESTRICT out_normals, const u16* ARTS_RESTRICT vertex_indices,
    const u8* ARTS_RESTRICT normals, u32 adjunct_count, Vector3* ARTS_RESTRICT accum, u32 vertex_count)
{
    for (u32 v = 0; v < vertex_count; ++v)
        accum[v] = {0.0f, 0.0f, 0.0f};

    for (u32 a = 0; a < adjunct_count; ++a)
        accum[vertex_indices[a]] += UnpackNormal[normals[a]];

    constexpr f32 kHardEdgeCos = 0.7f;

    for (u32 a = 0; a < adjunct_count; ++a)
    {
        const Vector3& own = UnpackNormal[normals[a]];
        const Vector3& sum = accum[vertex_indices[a]];

        f32 mag2 = sum.Mag2();

        if (mag2 <= 1.0e-8f)
        {
            out_normals[a] = own;
            continue;
        }

        Vector3 averaged = sum / std::sqrt(mag2);

        // Keep the facet's own normal wherever smoothing would round off a real edge.
        out_normals[a] = ((averaged ^ own) >= kHardEdgeCos) ? averaged : own;
    }
}

// Bind-pose vertex normals for a skinned model, rebuilt from the mesh's own geometry.
//
// This is what makes a pedestrian hash to ONE mesh instead of one per animation frame, and it is
// worth being precise about why the obvious source will not do.
//
// A pedestrian has no normals of its own. mmInstance::InitMeshes only asks for MESH_SET_NORMAL on
// COLLIDER and MOVER instances, so agiMeshSet::Normals is null for one, and the normals it is drawn
// with come from agiLitAnimation::Frames[frame] - one packed set PER ANIMATION FRAME, already posed.
// Submitting those means the normal bytes of the vertex change every time the walk cycle advances,
// and RTX Remix hashes the vertex bytes: same positions, same colours, same UVs, new mesh, sixty
// times a second. That is the -ghashcolor tint strobing on pedestrians and nothing else in the
// vertex was ever responsible - it is why the earlier work on positions, correct as far as it went,
// did not settle the churn.
//
// Unposing them does not fix it either, which is the trap here. The unposed value is *approximately*
// the bind-pose normal, but it is a 198-entry quantised direction rotated by a matrix rebuilt from
// the frame's pose, so it lands on slightly different floats for every frame of the animation. The
// hash needs bit-identical bytes, not a close answer.
//
// So the normals are computed from data that has no frame in it at all: the mesh's own positions and
// facets. Both are immutable, so this returns the same bytes for the life of the process, in every
// pose of every animation the model is played through - and, unlike the packed set, it is also a
// better normal, being a real area-weighted average rather than an 8-bit index into a coarse table.
//
// The vertices are in BONE-LOCAL space (agiMeshModel::ModelGeometry computes vertex * bone, with no
// inverse-bind step), so a facet's corners are only in a common frame when they share a bone. Those
// are the only facets used, which costs nothing in practice - a model is checked for exactly that
// property before this path is chosen - and a normal computed this way is in the same space as the
// positions beside it, so the palette matrix poses both together and no unposing is needed anywhere.
//
// The one thing geometry cannot supply is which side of the plane is outward: a cross product
// defines the plane, and the winding convention that would settle the rest is not something to bet a
// pedestrian's shading on. That is resolved by voting the geometric normals against the animation's
// stored ones, and it is the single place a per-frame input is consulted - deliberately reduced to
// ONE BIT for the whole submission, over hundreds of facets, so that it cannot vary between frames:
// a per-facet vote would put the frame back into the vertex bytes, one facet at a time.
static void BuildSkinBindNormals(Vector3* ARTS_RESTRICT out_normals, Vector3* ARTS_RESTRICT accum,
    const agiNativeSkinPalette& skin, const Vector3* ARTS_RESTRICT vertices, const u16* ARTS_RESTRICT vertex_indices,
    const u16* ARTS_RESTRICT surface_indices, const u8* ARTS_RESTRICT packed_normals, u32 surface_count,
    u32 adjunct_count, u32 vertex_count, const Vector3& filler)
{
    for (u32 v = 0; v < vertex_count; ++v)
        accum[v] = {0.0f, 0.0f, 0.0f};

    f32 vote = 0.0f;

    for (u32 facet = 0; facet < surface_count; ++facet)
    {
        const u16* ARTS_RESTRICT surface = &surface_indices[facet * 4];
        const u32 corners = surface[3] ? 4u : 3u;

        const u32 v0 = vertex_indices[surface[0]];
        const u8 bone = skin.VertexBones[v0];

        bool coherent = true;

        for (u32 k = 1; k < corners; ++k)
        {
            if (skin.VertexBones[vertex_indices[surface[k]]] != bone)
            {
                coherent = false;
                break;
            }
        }

        // A facet spanning two bones has its corners in two different local frames, so the triangle
        // it describes is not a triangle anywhere. It contributes no normal; its corners take theirs
        // from the facets around them, which do share their bone.
        if (!coherent)
            continue;

        const Vector3& p0 = vertices[v0];
        const Vector3& p1 = vertices[vertex_indices[surface[1]]];
        const Vector3& p2 = vertices[vertex_indices[surface[2]]];

        // Deliberately not normalised: the magnitude of the cross product is twice the triangle's
        // area, so accumulating it weights each facet by how much of the surface it actually is.
        // Normalising first would let a sliver of a facet pull a vertex normal as hard as the large
        // one next to it.
        Vector3 face;
        face.Cross(p1 - p0, p2 - p0);

        for (u32 k = 0; k < corners; ++k)
            accum[vertex_indices[surface[k]]] += face;

        // The sign vote. Bones outside this chunk's palette have no unpose matrix here, so they sit
        // it out; every chunk still votes over its own bones, which is far more than enough.
        const u32 slot = static_cast<u32>(bone) - skin.FirstBone;

        if (slot >= skin.Count)
            continue;

        Vector3 reference {};

        for (u32 k = 0; k < corners; ++k)
            reference += UnpackNormal[packed_normals[surface[k]]];

        Vector3 unposed;
        unposed.Dot3x3(reference, skin.NormalUnpose[slot]);

        vote += face ^ unposed;
    }

    const f32 sign = (vote < 0.0f) ? -1.0f : 1.0f;

    for (u32 a = 0; a < adjunct_count; ++a)
    {
        const Vector3& sum = accum[vertex_indices[a]];

        const f32 mag2 = sum.Mag2();

        // A vertex no usable facet reached - every facet touching it spans two bones, or has no
        // area. Rare, and a flat-lit vertex is a far smaller error than an unstable one.
        out_normals[a] = (mag2 > 1.0e-12f) ? (sum * (sign / std::sqrt(mag2))) : filler;
    }
}

b32 agiMeshSet::DrawNativeTransform(u32 flags, bool static_lighting, const agiNativeMaterialFx* fx,
    const u32* base_colors, bool unlit, const agiNativeSkinPalette* skin)
{
    Init((Planes != nullptr) && (SurfaceCount > 1));

    // NOTE: deliberately NOT doing a CPU-side bounding-box pre-cull here (the old path uses
    // TransformOutcode(), which transforms via the shared static agiMeshSet::M - refreshed by
    // the still-closed Init() above from *some* snapshot of the current transform). That's fine
    // for the old path, which uses the exact same M for its actual per-vertex transform right
    // afterward, so any staleness is self-consistent. This path instead transforms via the
    // hardware pipeline using ViewParams().World/View directly (set moments ago by this
    // object's own SetWorld() call, e.g. once per wheel for a car's four wheels drawn back to
    // back) - mixing that with an M-based pre-cull risks M lagging behind by one SetWorld() call
    // (e.g. still reflecting the previous wheel's transform), which would wrongly test a small,
    // corner-offset object's bounding box against the wrong frustum position and silently drop
    // it before ever reaching MeshWorld() - a clean, total disappearance rather than a visible
    // glitch. The GPU clips real off-screen geometry correctly regardless, so skipping this
    // optimization only costs a bit of CPU work building vertex/index data for meshes that
    // might be off-screen - never a correctness problem.

    DynTexFlag = flags & MESH_DRAW_DYNTEX;
    CurrentMeshSetVariant = std::min<i32>(MESH_DRAW_GET_VARIANT(flags), VariationCount - 1);

    // So the renderer can tell which glow lights can reach this mesh - see agiworld/glowlight.h.
    agiNativeDrawRadius = Radius;

    // Facet order for this submission, in canonical form: every facet, grouped by texture, in
    // ascending facet index. This is what makes the geometry hash RTX Remix computes from the
    // submitted index bytes a property of the MESH rather than of the moment it was drawn, and two
    // separate things about the code it replaces made that false.
    //
    // 1. It was CAMERA DEPENDENT. Facets were filtered through IsBackfacing(), which is
    //    plane . EyePos + plane.w tested against MirrorMode, with EyePos the camera expressed in
    //    model space and refreshed by the Init() above on every SetWorld(). Remix keys a
    //    replacement on the bytes of the draw, indices included, so a building emitted a different
    //    index array - and therefore took on a different identity - for every position the camera
    //    moved to. Replacements never stick and a capture fills with thousands of one-frame meshes.
    //
    //    The filter also switched on and off underneath this path, which produced whole additional
    //    families of hashes for one mesh: AllowEyeBackfacing is raised only by InitMtx, and only
    //    when Planes && SurfaceCount > 1, MirrorMode is clear, and the transform passes InitMtx's
    //    scale tolerance (game.asm ~324504) - it is cleared on every other path (~324569) - so the
    //    same mesh alternated between "every facet" and "some facets", and the rear-view mirror,
    //    which sets MirrorMode and so inverts the comparison, contributed the complementary set on
    //    top of that.
    //
    //    Dropping the test costs nothing, because the GPU already culls these facets by winding:
    //    MeshWorld() programs D3DRS_CULLMODE from agiCurState through ToD3DCullFlipped(). Nor was
    //    the CPU test ever load-bearing here - for every draw where AllowEyeBackfacing is false,
    //    which is most of them, it culled nothing at all and the winding cull was doing the whole
    //    job unaided. It is a saving rather than a cost: one plane dot product per facet per draw
    //    goes away, and a path tracer wants closed geometry regardless, because a back face it
    //    never receives is a hole light leaks through.
    //
    // 2. It was built in the SHARED static firstFacet/nextFacet scratch. Those are process-wide
    //    arrays (the assembly exports them), and this function is reached on more than one thread -
    //    the same fact documented for the vertex buffers in the ARTS_ALLOCA note below. Two
    //    concurrent draws therefore interleaved their chains, and the index array that came out was
    //    not a function of either mesh. That is unhashable no matter what happens to the cull, so
    //    the order is built in local storage instead.
    //
    //    Nothing downstream loses anything by that. The closed routines that do consume that
    //    scratch - SphereMap(), EnvMap(), MultiTexEnvMap() - only ever run after Geometry(), which
    //    builds its own chains, and are never reached from this path (see DrawLitEnv).
    //
    // What is left is a pure function of SurfaceIndices and TextureIndices, both immutable mesh
    // data, so a given mesh submits byte-identical indices for the life of the process. There is
    // deliberately no cross-frame cache of the result: agiMeshSet's layout is fixed by
    // check_size(agiMeshSet, 0x64) so the arrays cannot live on the mesh, and a side table keyed on
    // the mesh pointer would have to outguess a lifetime the assembly partly owns - for a saving of
    // one linear pass. If this backend ever grows device-owned dynamic vertex/index buffers in
    // place of DrawIndexedPrimitiveUP, those are the right owner for it: they would have exactly
    // the BeginGfx/EndGfx lifetime such a cache needs.
    //
    // The facet index is emitted as u16 because the engine's own facet storage is i16 (nextFacet is
    // i16[16384]), so a mesh above that has never been submittable by any path here.
    if (SurfaceCount > 0x7FFFu)
        return false;

    const bool cpu_cull = (Planes != nullptr) && PARAM_native_cpu_cull.get_or(false);

    // A chunk of a skinned model takes only the facets it wholly owns. When the palette holds the
    // whole skeleton - the usual case - that is every facet and this excludes nothing. When the
    // skeleton had to be split, a facet spanning the boundary would tear, so it is left out and
    // agiMeshModel::ModelDrawLit only splits a model it has checked has no such facet.
    const auto facet_excluded = [&](u32 facet) {
        if (cpu_cull && IsBackfacing(Planes[facet]))
            return true;

        if (!skin)
            return false;

        const u16* ARTS_RESTRICT surface = &SurfaceIndices[facet * 4];
        const u32 corners = surface[3] ? 4u : 3u;

        for (u32 k = 0; k < corners; ++k)
        {
            const u32 vertex = VertexIndices[surface[k]];

            if ((vertex < skin->FirstVertex) || (vertex >= skin->EndVertex))
                return true;
        }

        return false;
    };

    const u32 batch_count = TextureCount + 1u;

    u32* batch_start = ARTS_ALLOCA(u32, batch_count + 1);
    u16* facet_order = ARTS_ALLOCA(u16, SurfaceCount);

    for (u32 i = 0; i <= batch_count; ++i)
        batch_start[i] = 0;

    for (u32 i = 0; i < SurfaceCount; ++i)
    {
        if (facet_excluded(i))
            continue;

        ++batch_start[TextureIndices[i] + 1u];
    }

    for (u32 i = 0; i < batch_count; ++i)
        batch_start[i + 1] += batch_start[i];

    {
        // Counting sort rather than the old prepend-to-a-linked-list, so facets come out in
        // ascending index order within their batch instead of reversed. Either is deterministic;
        // ascending matches the order they appear in SurfaceIndices, which is the order a capture
        // and any tooling built on it will read them in.
        u32* cursor = ARTS_ALLOCA(u32, batch_count);

        for (u32 i = 0; i < batch_count; ++i)
            cursor[i] = batch_start[i];

        for (u32 i = 0; i < SurfaceCount; ++i)
        {
            if (facet_excluded(i))
                continue;

            facet_order[cursor[TextureIndices[i]]++] = static_cast<u16>(i);
        }
    }

    // Index buffer size is NOT IndicesCount. That is the length of the *source* SurfaceIndices
    // array - 4 entries per facet (agiworld/meshload.cpp reads SurfaceIndices as IndicesCount
    // u16s) - whereas the loop below emits triangles: 3 indices for a triangle facet but 6 for a
    // quad. A quad-heavy mesh therefore needs 1.5x what IndicesCount describes, so sizing by it
    // overran the buffer and corrupted whatever followed. SurfaceCount * 6 is the true worst case.
    const u32 max_indices = SurfaceCount * 6;

    // ARTS_ALLOCA, deliberately - do NOT move these buffers onto the heap. Three revisions have now
    // tried it and all three failed:
    //
    // - `static std::vector` raced. This function is reached on more than one thread (the device is
    //   created D3DCREATE_MULTITHREADED), so one thread's resize() reallocated the storage out from
    //   under another thread's already-taken pointer.
    // - `static thread_local std::vector` removed the race and still failed, which rules the race
    //   out as the whole story. It crashed on entering gameplay, inside the *simulation* loop, with
    //   AI vehicle fields reading 0x55555555 - asMemoryAllocator::Node::LOWER_FILL, the custom
    //   allocator's guard byte (memory/allocator.cpp). Bisected with OPEN1560_NATIVE_MASK: mask 0
    //   ran clean, mask 0x1 (this path via Draw()) reproduced it every time, and reverting to
    //   ARTS_ALLOCA with nothing else changed fixed it. Whatever the precise mechanism, calling the
    //   game's own allocator from this point in the draw is not safe.
    //
    // The cost of staying on the stack is a real but currently theoretical overflow risk: meshes
    // may reach BigVtxSize (16384) adjuncts, and at sizeof(agiWorldVtx) == 0x24 that is ~590 KB of
    // vertices plus up to ~192 KB of indices, per call, in an already-deep draw chain against a
    // 1 MB stack. Real meshes are far smaller and this has not been observed to fire. If it ever
    // needs fixing, the fix is a buffer that is neither the game heap nor the stack - a
    // module-level array sized once for the worst case, or device-owned dynamic D3D9 vertex and
    // index buffers - not std::vector.
    //
    const agiViewParameters& view_params = ViewParams();

    // Meshes loaded without MESH_SET_NORMAL have no normals at all (mmInstance::InitMeshes only
    // requests them for COLLIDER/MOVER instances, so most static scenery and the low-detail LODs
    // go without). Those draw unlit from their baked Colors on the CPU path, and do the same here -
    // the normal field is filler and MeshWorld() is told to disable hardware lighting, so nothing
    // reads it. Without this they would fall back to CPU pretransform and stay invisible to Remix.
    const bool has_normals = (Normals != nullptr);
    const bool hardware_lighting = has_normals && !unlit;
    const Vector3 filler_normal {0.0f, 1.0f, 0.0f};

    const u32* src_colors = base_colors ? base_colors : Colors;

    // -flatnormals. Throw away the mesh's per-vertex normals and shade from the geometry instead.
    //
    // The stored normals are the least trustworthy thing in a .bms. They are 8-bit indices into a
    // 198-entry table (agiworld/packnorm.h), so they are coarse to begin with, and they are stored
    // per ADJUNCT while the positions are per VERTEX - so a model with coincident or otherwise
    // stray vertices can carry a normal that points nowhere near the surface it is attached to.
    // SmoothAdjunctNormals makes that worse rather than better where it fires, because it averages
    // across every adjunct sharing a vertex position: two surfaces that merely happen to touch get
    // blended into each other, which is the "wrongly shaded in some areas" look on city geometry.
    //
    // Flat mode sidesteps the stored normals entirely. Each facet gets the true normal of its own
    // plane, computed from its own corner positions, and its corners are emitted as UNSHARED
    // vertices so no facet can contaminate its neighbour. Nothing about the result depends on the
    // vertex data being sane - only on the triangle having area.
    //
    // The sign still comes from the stored normals, deliberately: a cross product only defines the
    // plane, not which side is outward, and the winding convention here is not something to bet on
    // (see the long note in agidx9's MeshWorld about exactly that). Agreeing with the average of the
    // facet's own stored normals keeps the artist's intended facing while replacing the direction,
    // and a stray normal cannot flip the result because the vote is over the whole facet.
    //
    // Off by default. It is a real change in look - genuinely faceted, no smooth shading anywhere -
    // and on well-formed meshes the stored normals are better.
    const u32 flat_capacity = SurfaceCount * 4;

    const bool flat_shading = has_normals && hardware_lighting && PARAM_flat_normals.get_or(false) &&
        (flat_capacity <= kFlatVertexCap) && (SurfaceCount > 0);

    // Flat mode needs one vertex per facet corner rather than one per adjunct, and both bounds are
    // stack allocations for the reason documented above.
    agiWorldVtx* verts = ARTS_ALLOCA(agiWorldVtx, flat_shading ? flat_capacity : AdjunctCount);

    // One palette slot per SUBMITTED vertex, built alongside them - see agiNativeSkinPalette::Slots.
    // _alloca is frame-scoped rather than block-scoped, so these outlive the branch they are taken
    // in.
    u8* skin_slots = nullptr;

    // Frame-independent normals for a skinned model - the whole reason a pedestrian hashes to one
    // mesh. See BuildSkinBindNormals. Built for the adjunct array in both shading modes, because
    // flat mode needs them for its sign reference even though it computes its own face normals.
    Vector3* bind_normals = nullptr;

    if (skin)
    {
        skin_slots = ARTS_ALLOCA(u8, flat_shading ? flat_capacity : AdjunctCount);

        if (has_normals)
        {
            bind_normals = ARTS_ALLOCA(Vector3, AdjunctCount);

            Vector3* bind_accum = ARTS_ALLOCA(Vector3, VertexCount);

            BuildSkinBindNormals(bind_normals, bind_accum, *skin, Vertices, VertexIndices, SurfaceIndices, Normals,
                SurfaceCount, AdjunctCount, VertexCount, filler_normal);
        }
    }

    // Mesh vertex -> palette slot. Out-of-chunk vertices land outside the palette and are clamped:
    // no index references them, but they are inside the vertex range the draw hands the device and
    // a matrix index past the end of the palette is undefined behaviour, not merely unused. The
    // subtraction is unsigned, so a vertex below the chunk wraps large and clamps by the same test.
    const auto skin_slot = [&](u32 vertex) -> u8 {
        const u32 slot = static_cast<u32>(skin->VertexBones[vertex]) - skin->FirstBone;

        return static_cast<u8>((slot < skin->Count) ? slot : 0u);
    };

    u16* facet_base = nullptr;
    u32 flat_vertex_count = 0;

    if (flat_shading)
    {
        facet_base = ARTS_ALLOCA(u16, SurfaceCount);

        // Walks facet_order, exactly as the index loop below does, so corner k of a facet is always
        // at facet_base[facet] + k. Sharing that one order is what keeps this path hashable too:
        // flat shading emits unshared vertices, so the vertex array is built here rather than from
        // the adjunct list, and it would inherit any instability in the order it walks.
        for (u32 slot = 0; slot < batch_start[batch_count]; ++slot)
        {
            const u16 facet = facet_order[slot];

            const u16* ARTS_RESTRICT surface = &SurfaceIndices[facet * 4];
            const u32 corners = surface[3] ? 4u : 3u;

            facet_base[facet] = static_cast<u16>(flat_vertex_count);

            Vector3 reference {};

            // The stored normals of a skinned model are posed for the frame while its positions are
            // not, so the two are in different spaces and this sign vote - a dot product between
            // them - would be comparing nothing. The bind normals are already in the positions'
            // space and already carry the resolved sign, so they are the reference there.
            for (u32 k = 0; k < corners; ++k)
                reference = reference + (bind_normals ? bind_normals[surface[k]] : UnpackNormal[Normals[surface[k]]]);

            const Vector3& p0 = Vertices[VertexIndices[surface[0]]];
            const Vector3& p1 = Vertices[VertexIndices[surface[1]]];
            const Vector3& p2 = Vertices[VertexIndices[surface[2]]];

            Vector3 face_normal;
            face_normal.Cross(p1 - p0, p2 - p0);

            if (f32 mag2 = face_normal.Mag2(); mag2 > 1.0e-12f)
            {
                face_normal = face_normal * (1.0f / std::sqrt(mag2));

                if ((face_normal ^ reference) < 0.0f)
                    face_normal = -face_normal;
            }
            else if (f32 ref_mag2 = reference.Mag2(); ref_mag2 > 1.0e-12f)
            {
                // Degenerate facet - zero area, so it has no plane of its own. It will not
                // rasterise anything either, but it still needs a finite normal.
                face_normal = reference * (1.0f / std::sqrt(ref_mag2));
            }
            else
            {
                face_normal = filler_normal;
            }

            for (u32 k = 0; k < corners; ++k)
            {
                const u16 a = surface[k];

                if (skin_slots)
                    skin_slots[flat_vertex_count] = skin_slot(VertexIndices[a]);

                agiWorldVtx& vertex = verts[flat_vertex_count++];

                vertex.pos = Vertices[VertexIndices[a]];
                vertex.normal = face_normal;
                vertex.color = src_colors ? src_colors[a] : 0xFFFFFFFF;
                vertex.tu = TexCoords ? TexCoords[a].x : 0.0f;
                vertex.tv = TexCoords ? TexCoords[a].y : 0.0f;
            }
        }
    }
    else
    {
        // Smoothed normals, when the mesh has any and is small enough to do it on the stack. See
        // SmoothAdjunctNormals - this is what stops low-poly bodywork looking flat-shaded regardless
        // of how the lighting is evaluated.
        Vector3* smooth_normals = nullptr;

        // Not for a skinned model: BuildSkinBindNormals has already averaged over the facets, from
        // the geometry rather than from the packed set, so this would be a second smoothing pass
        // over an input that no longer exists.
        if (!skin && has_normals && PARAM_smooth_normals.get_or(true) && (VertexCount <= 4096) &&
            (AdjunctCount <= 4096))
        {
            smooth_normals = ARTS_ALLOCA(Vector3, AdjunctCount);
            Vector3* accum = ARTS_ALLOCA(Vector3, VertexCount);

            SmoothAdjunctNormals(smooth_normals, VertexIndices, Normals, AdjunctCount, accum, VertexCount);
        }

        for (u32 a = 0; a < AdjunctCount; ++a)
        {
            const u32 vertex_index = VertexIndices[a];

            verts[a].pos = Vertices[vertex_index];

            if (skin_slots)
                skin_slots[a] = skin_slot(vertex_index);

            // bind_normals first: for a skinned model it is the only source that does not carry the
            // animation frame into the vertex bytes.
            Vector3 normal = filler_normal;

            if (bind_normals)
                normal = bind_normals[a];
            else if (has_normals)
                normal = smooth_normals ? smooth_normals[a] : UnpackNormal[Normals[a]];

            verts[a].normal = normal;
            verts[a].color = src_colors ? src_colors[a] : 0xFFFFFFFF;
            verts[a].tu = TexCoords ? TexCoords[a].x : 0.0f;
            verts[a].tv = TexCoords ? TexCoords[a].y : 0.0f;
        }
    }

    u16* indices = ARTS_ALLOCA(u16, max_indices);

    // Vehicle chrome. DrawLitSph cannot pass an fx down - it reaches here through DrawLit, whose
    // signature the assembly owns - so it leaves the subject in agiNativeReflectivity /
    // agiNativeReflectionTex and this assembles the fx. Same reason agiNativeDrawRadius exists.
    //
    // Gated on has_normals, and that gate is the whole point rather than defensive coding: the
    // sphere map is indexed by the reflection of the eye vector about the vertex normal, so a mesh
    // with no normals would reflect the same texel over its entire surface - a flat wash of one
    // colour, which looks worse than no chrome at all.
    agiNativeMaterialFx native_fx {};
    const agiNativeMaterialFx* effective_fx = fx;

    // Diagnostics. "Offered" counts draws that arrived with a sphere map selected, so
    // offered == 0 means no caller is asking for chrome at all and the problem is upstream of here;
    // offered > 0 with drawn == 0 means it is being asked for and refused, and the reason is the
    // missing-normals count next to it.
    const bool reflect_offered = !effective_fx && agiNativeReflectionTex && (agiNativeReflectivity > 0.0f);

    if (reflect_offered && !has_normals)
        ++agiReflectSkipNoNormals;

    if (reflect_offered && has_normals)
    {
        ++agiReflectDraws;

        native_fx.ReflectionTexture = agiNativeReflectionTex;
        native_fx.ReflectionAmount = agiNativeReflectivity * PARAM_reflect_amount.get_or(0.35f);
        native_fx.FresnelBias = PARAM_reflect_fresnel_bias.get_or(1.0f);
        native_fx.FresnelScale = PARAM_reflect_fresnel_scale.get_or(0.0f);
        native_fx.SpecularBoost = PARAM_reflect_specular.get_or(0.0f);

        effective_fx = &native_fx;
    }

    // The palette as the renderer receives it: the caller's, plus the slot table built above. Kept
    // as a local copy so the caller's struct - which describes the model, not this submission - is
    // not written through.
    agiNativeSkinPalette draw_skin {};

    if (skin)
    {
        draw_skin = *skin;
        draw_skin.Slots = skin_slots;
    }

    bool drawn = false;
    u32 submitted_indices = 0;

    for (u32 texture = 0; texture < batch_count; ++texture)
    {
        const u32 batch_end = batch_start[texture + 1];

        if (batch_start[texture] == batch_end)
            continue;

        u32 index_count = 0;

        for (u32 slot = batch_start[texture]; slot < batch_end; ++slot)
        {
            const u16 facet = facet_order[slot];

            const u16* ARTS_RESTRICT surface = &SurfaceIndices[facet * 4];

            // Flat mode emitted this facet's corners as its own unshared run of vertices, in
            // corner order, so corner k is at facet_base[facet] + k. Otherwise the adjunct index
            // in SurfaceIndices is already the vertex index.
            const u16 base = flat_shading ? facet_base[facet] : u16 {0};

            const u16 c0 = flat_shading ? static_cast<u16>(base + 0) : surface[0];
            const u16 c1 = flat_shading ? static_cast<u16>(base + 1) : surface[1];
            const u16 c2 = flat_shading ? static_cast<u16>(base + 2) : surface[2];

            if (surface[3])
            {
                const u16 c3 = flat_shading ? static_cast<u16>(base + 3) : surface[3];

                // Matches the diagonal split ClipTri uses for quads elsewhere in this file.
                indices[index_count++] = c1;
                indices[index_count++] = c2;
                indices[index_count++] = c3;
                indices[index_count++] = c1;
                indices[index_count++] = c3;
                indices[index_count++] = c0;
            }
            else
            {
                indices[index_count++] = c0;
                indices[index_count++] = c1;
                indices[index_count++] = c2;
            }
        }

        if (index_count == 0)
            continue;

        agiTexDef* tex_def = TexCoords ? Textures[CurrentMeshSetVariant][texture] : nullptr;

        auto old_texture = agiCurState.SetTexture(tex_def);

        const u32 vertex_count = flat_shading ? flat_vertex_count : AdjunctCount;

        if (RAST->MeshWorld(verts, static_cast<i32>(vertex_count), indices, static_cast<i32>(index_count),
                skin ? skin->Bones[0] : view_params.World, view_params.View, view_params, static_lighting, effective_fx,
                hardware_lighting, skin ? &draw_skin : nullptr))
        {
            drawn = true;
            submitted_indices += index_count;
        }

        agiCurState.SetTexture(old_texture);
    }

    if (drawn)
    {
        const u32 tris = submitted_indices / 3;

        ++agiMeshNormalDraws;
        agiMeshNormalTris += tris;

        if (!has_normals)
        {
            ++agiMeshNormalDrawsFlat;
            agiMeshNormalTrisFlat += tris;
        }
    }

    return drawn;
}

i32 agiMeshSet::ShadowGeometry(u32 flags, Vector3* verts, const Vector4& plane, const Vector3& light_dir)
{
    ClippedVertCount = 0;
    ClippedTriCount = 0;
    ClippedTextures[0] = 0;
    ShadowInit(plane, light_dir);

    u32 clip_mask = (flags & MESH_DRAW_CLIP) ? MESH_CLIP_ANY : 0; // clip_any | (clip_all << 8)

    {
        ARTS_UTIMED(agiTransformTimer);

        // If available, transform the bounding box, checking how visible it is
        if (clip_mask && BoundingBox)
        {
            clip_mask = ShadowTransformOutcode(codes, out, BoundingBox, 8);

            if (clip_mask > 0xFF) // All verts are clipped (not visible)
                return clip_mask;
        }

        if (clip_mask)
        {
            clip_mask = ShadowTransformOutcode(codes, out, verts, VertexCount);

            if (clip_mask > 0xFF) // All verts are clipped (not visible)
                return clip_mask;
        }
        else // Assume nothing needs clipping
        {
            ShadowTransform(out, verts, VertexCount);
        }
    }

    {
        ARTS_UTIMED(agiTraverseTimer);

        DynTexFlag = 0;
        CurrentMeshSetVariant = 0;

        firstFacet[0] = -1;
        vertCounts[0] = 0;
        indexCounts[0] = 0;

        if (clip_mask)
        {
            for (u32 i = 0; i < SurfaceCount; ++i)
            {
                u16 surface[4];
                std::memcpy(surface, &SurfaceIndices[i * 4], sizeof(surface));

                u16 indices[4];
                indices[0] = VertexIndices[surface[0]];
                indices[1] = VertexIndices[surface[1]];
                indices[2] = VertexIndices[surface[2]];

                u8 clip_any = codes[indices[0]] | codes[indices[1]] | codes[indices[2]];
                u8 clip_all = codes[indices[0]] & codes[indices[1]] & codes[indices[2]];

                i16 num_verts;
                i16 num_index;

                if (surface[3])
                {
                    indices[3] = VertexIndices[surface[3]];
                    clip_any |= codes[indices[3]];
                    clip_all &= codes[indices[3]];
                    num_verts = 4;
                    num_index = 6;
                }
                else
                {
                    num_verts = 3;
                    num_index = 3;
                }

                if (!(clip_all & MESH_CLIP_ANY))
                {
                    if (clip_any & ClipMask)
                    {
                        if (surface[3])
                        {
                            ClipTri(surface[1], surface[2], surface[3], 0);
                            ClipTri(surface[1], surface[3], surface[0], 0);
                        }
                        else
                        {
                            ClipTri(surface[0], surface[1], surface[2], 0);
                        }
                    }
                    else
                    {
                        vertCounts[0] += num_verts;
                        indexCounts[0] += num_index;
                        nextFacet[i] = firstFacet[0];
                        firstFacet[0] = static_cast<i16>(i);
                    }

                    codes[indices[0]] |= MESH_CLIP_SCREEN;
                    codes[indices[1]] |= MESH_CLIP_SCREEN;
                    codes[indices[2]] |= MESH_CLIP_SCREEN;

                    if (surface[3])
                        codes[indices[3]] |= MESH_CLIP_SCREEN;
                }
            }
        }
        else
        {
            // Either we didn't use TransformOutcode, or we don't care about the clipping it returned. Either way, initialize codes
            fill_bytes(codes, VertexCount,
#ifdef CLIP_ALL_TO_SCREEN
                MESH_CLIP_SCREEN
#else
                0
#endif
            );

            for (u32 i = 0; i < SurfaceCount; ++i)
            {
                const u16* ARTS_RESTRICT surface = &SurfaceIndices[i * 4];

                if (surface[3])
                {
                    vertCounts[0] += 4;
                    indexCounts[0] += 6;

#ifndef CLIP_ALL_TO_SCREEN
                    codes[VertexIndices[surface[3]]] |= MESH_CLIP_SCREEN;
#endif
                }
                else
                {
                    vertCounts[0] += 3;
                    indexCounts[0] += 3;
                }

#ifndef CLIP_ALL_TO_SCREEN
                codes[VertexIndices[surface[0]]] |= MESH_CLIP_SCREEN;
                codes[VertexIndices[surface[1]]] |= MESH_CLIP_SCREEN;
                codes[VertexIndices[surface[2]]] |= MESH_CLIP_SCREEN;
#endif

                nextFacet[i] = firstFacet[0];
                firstFacet[0] = static_cast<i16>(i);
            }
        }
    }

    ToScreen(codes, out, VertexCount);

    return clip_mask;
}

// ?CurrentMeshCard@@3UagiMeshCardInfo@@A
ARTS_IMPORT extern agiMeshCardInfo CurrentMeshCard;

// --- Glow light harvesting (see agiworld/glowlight.h) -----------------------------------------

agiGlowLight agiGlowLights[AGI_MAX_GLOW_LIGHTS] {};
u32 agiGlowLightCount = 0;

bool agiGlowHarvestEnabled = false;

// Source of agiGlowLight::Id. Process-lifetime and never reset, so an id is never handed out twice -
// a consumer holding state for a light that has since expired can never mistake a new one for it.
static u32 s_next_glow_light_id = 0;

u32 agiGlowCardsSeen = 0;
u32 agiGlowCardsNoTexture = 0;
u32 agiGlowCardsNotGlow = 0;
u32 agiGlowCardsHarvested = 0;

f32 agiNativeDrawRadius = 0.0f;
f32 agiLightningFlash = 0.0f;

// NOTE: at namespace scope deliberately. A function-local `static mem::cmd_param` is constructed
// lazily on first call, which happens long after mem::cmd_param::init() has already walked argv
// and assigned values to every registered parameter - so it registers too late and silently
// never receives its value, no matter what the user passes on the command line.
static mem::cmd_param PARAM_glowdebug {"glowdebug", "Log glow lights as they are harvested"};

// Per-kind intensity, because these are not one population.
//
// The engine gives no explicit "what sort of light is this" field, but the glow textures are named
// descriptively and their vertex tints are very distinct, which between them identify every case
// seen in the draw stream:
//
//   FXLTCONE      - the headlight cone mesh. Huge (radius ~39) and centred well ahead of the car.
//   FXLTGLOWRED   - vehicle tail/brake, tinted dark red and sitting near ground level.
//   FXLTGLOW      - shared between traffic signals and street lamps. They are trivially separable
//                   by saturation: a signal is a pure hue (green measured as 0.00/1.00/0.44), a
//                   lamp is warm near-white (1.00/0.98/0.47). Nothing else in the stream sits
//                   between those.
//
// Every multiplier is a cmd_param so this can be tuned without a rebuild, and so the classification
// being heuristic does not lock anyone into my guesses.
static mem::cmd_param PARAM_light_head {"lighthead", "Intensity of vehicle headlight cones"};
static mem::cmd_param PARAM_light_vehicle {"lightvehicle", "Intensity of vehicle tail/brake/reverse lights"};
static mem::cmd_param PARAM_light_traffic {"lighttraffic", "Intensity of traffic signals"};
static mem::cmd_param PARAM_light_lamp {"lightlamp", "Intensity of street lamps and static lights"};
static mem::cmd_param PARAM_light_generic {
    "lightgeneric", "Intensity of neutral-white glows that are not street lamps"};

// Relative saturation above which a colour counts as a pure signal hue rather than a warm white.
//
// This test used to be an ABSOLUTE channel spread, `(peak - floor) > 0.40`, and it misclassified the
// exact case documented above it. A street lamp measures 1.00/0.98/0.47, whose absolute spread is
// 0.53 - so every street lamp in the city tested as "saturated", fell through to the traffic-signal
// branch, and ran at `lighttraffic` (2.0) instead of `lightlamp` (10.0). Warm white is by definition
// a wide absolute spread, so an absolute threshold can never separate it from a hue.
//
// It failed the other way too, because the tint handed in has already been multiplied by the flare's
// brightness (agiAddGlowLight folds in the card's alpha). A genuinely red tail light at alpha 0.3
// arrives as 0.30/0.00/0.00, an absolute spread of 0.30 - "unsaturated" - and was classified as a
// street lamp at 10.0. The two populations were being swapped in both directions, and which way a
// given flare went depended on how bright it happened to be drawn that frame.
//
// Relative saturation (peak - floor)/peak is scale-invariant, which removes the brightness
// dependence outright, and 0.65 separates the observed populations with room to spare:
//   street lamp   1.00/0.98/0.47 -> 0.53   warm white
//   green signal  0.00/1.00/0.44 -> 1.00   hue
//   red tail      1.00/0.10/0.10 -> 0.90   hue
//   amber signal  1.00/0.60/0.10 -> 0.90   hue
static constexpr f32 kGlowHueSaturation = 0.65f;

// Per-kind on/off, so a light source can be removed from the scene entirely rather than merely
// dimmed. Setting a kind's intensity to 0 leaves it occupying a pool slot and a cell-grid entry for
// nothing; these are checked at harvest time, so a disabled kind never enters the registry at all.
//
// The kinds are the ones the classifier can genuinely tell apart from the draw stream. Brake and
// tail lights are deliberately NOT separate switches: both are FXLTGLOWRED on the same mesh and the
// engine gives no state flag distinguishing them, so a "brakes only" toggle would be a guess
// dressed up as a setting. Reverse lamps come out as neutral whites and so live under `generic`.
static mem::cmd_param PARAM_glow_on_head {"glowheadlights", "Headlight cones emit light"};
static mem::cmd_param PARAM_glow_on_vehicle {"glowvehiclelights", "Vehicle tail and brake lights emit light"};
static mem::cmd_param PARAM_glow_on_traffic {"glowtrafficlights", "Traffic signals emit light"};
static mem::cmd_param PARAM_glow_on_lamp {"glowstreetlamps", "Street lamps emit light"};
static mem::cmd_param PARAM_glow_on_generic {
    "glowgenericlights", "Neutral white glows (reverse lamps, coronas) emit light"};

bool agiGlowKindEnabled(agiGlowKind kind)
{
    switch (kind)
    {
        case agiGlowKind::Headlight: return PARAM_glow_on_head.get_or(true);
        case agiGlowKind::Vehicle: return PARAM_glow_on_vehicle.get_or(true);
        case agiGlowKind::Traffic: return PARAM_glow_on_traffic.get_or(true);
        case agiGlowKind::Lamp: return PARAM_glow_on_lamp.get_or(true);
        default: return PARAM_glow_on_generic.get_or(true);
    }
}

// The classification itself, split out so intensity and the on/off switch cannot disagree about
// what a light is - which is exactly the failure the saturation note below describes.
agiGlowKind agiClassifyGlowKind(const char* name, const Vector3& color)
{
    if (!name)
        return agiGlowKind::Generic;

    if (std::strstr(name, "CONE"))
        return agiGlowKind::Headlight;

    if (std::strstr(name, "FXLTGLOWRED") || std::strstr(name, "FXLTGLOWAMBER"))
        return agiGlowKind::Vehicle;

    const f32 peak = std::max({color.x, color.y, color.z});

    if (peak <= 1e-4f)
        return agiGlowKind::Lamp;

    const f32 floor_ = std::min({color.x, color.y, color.z});

    if (((peak - floor_) / peak) > kGlowHueSaturation)
        return agiGlowKind::Traffic;

    if ((color.z / peak) < 0.85f)
        return agiGlowKind::Lamp;

    return agiGlowKind::Generic;
}

f32 agiClassifyGlowIntensity(const char* name, const Vector3& color)
{
    if (!name)
        return 1.0f;

    // Headlight cones. These used to be pulled back to 0.05, because the light sat at the centroid of
    // the cone mesh - metres ahead of the bonnet - and washed the road from the wrong place. The
    // harvest now puts it at the lamp and aims it down the beam (HarvestHeadlightBeam, dx9rsys.cpp),
    // and its reach is the beam's full length. That reach is large - the cone is tens of units long -
    // and brightness goes with its square, so a modest multiplier already makes a beam that lights the
    // road ten units ahead about as strongly as a street lamp lights the pavement under it.
    if (std::strstr(name, "CONE"))
        return PARAM_light_head.get_or(2.0f);

    // Name first where the name is decisive. FXLTGLOWRED/AMBER are vehicle lamp sheets whatever
    // colour the instance tints them, so they never need the saturation test at all.
    if (std::strstr(name, "FXLTGLOWRED") || std::strstr(name, "FXLTGLOWAMBER"))
        return PARAM_light_vehicle.get_or(1.25f);

    const f32 peak = std::max({color.x, color.y, color.z});

    // A black flare has no hue to classify. It also emits nothing, so the value is academic.
    if (peak <= 1e-4f)
        return PARAM_light_lamp.get_or(10.0f);

    const f32 floor_ = std::min({color.x, color.y, color.z});
    const f32 saturation = (peak - floor_) / peak;

    // A pure hue is a signal: a traffic light, or a lamp whose whole job is to be looked at.
    if (saturation > kGlowHueSaturation)
        return PARAM_light_traffic.get_or(2.0f);

    // Unsaturated splits again, on WARMTH.
    //
    // "Not a pure hue" is not sufficient to be a street lamp, and treating it as such handed the
    // full lamp multiplier to things that are not lamps - measured in the log as a pure white
    // (1.00 1.00 1.00) glow sitting at y=0.3, i.e. on a vehicle, being given intensity 25. Anything
    // that big at ground level washes out the road around a car.
    //
    // Every street lamp in this game is warm - incandescent or sodium - so its blue channel sits
    // well under its red. A neutral white glow is something else (a reverse lamp, a generic corona)
    // and gets a modest default rather than a street lamp's budget.
    if ((color.z / peak) < 0.85f)
        return PARAM_light_lamp.get_or(10.0f);

    return PARAM_light_generic.get_or(1.0f);
}

static mem::cmd_param PARAM_glow_reach_scale {
    "glowreachscale", "Glow flare half-extent to light reach, in world units"};
static mem::cmd_param PARAM_glow_reach_min {"glowreachmin", "Minimum reach of a glow-driven light, in world units"};

f32 agiGlowLightReach(f32 flare_half_extent)
{
    return std::max(flare_half_extent * PARAM_glow_reach_scale.get_or(14.0f), PARAM_glow_reach_min.get_or(20.0f));
}

void agiAddGlowLightRGB(const Vector3& position, const Vector3& tint, f32 radius, agiTexDef* texture, f32 u, f32 v,
    const Vector3& direction, f32 cone_angle, const Vector3* local)
{
    if ((tint.x <= 0.0f) && (tint.y <= 0.0f) && (tint.z <= 0.0f))
        return;

    // Find the slot this sprite already owns, so it is refreshed in place rather than duplicated.
    //
    // Matching is against the slot's PREDICTED position - where last frame's velocity says it should
    // be now - not against where it was. A plain distance match fails exactly when it matters most:
    // a car at 200 km/h covers about a metre per frame, so a one-metre threshold stopped recognising
    // its own headlights, allocated a fresh slot every frame, and the orphans then survived their
    // full TTL. That is a trail of lights strung out behind the car, which is what was reported.
    // Predicting first makes the residual near zero at constant speed, so the threshold only has to
    // absorb acceleration rather than the whole of the motion.
    //
    // Slots already refreshed this frame (Age == 0) are skipped. Without that guard the threshold -
    // which now has to be generous - would let a car's second headlight claim the slot its first one
    // just took, collapsing a pair of lights into one.
    // 0.9 m. Prediction absorbs the motion, so this only has to cover acceleration - and keeping it
    // well under the ~1.5 m spacing between a car's two tail lights stops them trading slots frame
    // to frame, which is what made them flicker at speed.
    //
    // That radius is still what a flare without a model-space position (see below) is matched with.
    constexpr f32 kMatchDist = 0.9f;
    constexpr f32 kMatchDistSq = kMatchDist * kMatchDist;

    // IDENTITY BY MODEL-SPACE POSITION, WHEN THE HARVEST KNOWS IT.
    //
    // Prediction alone was not enough. It assumes every frame is the same length and that the lamp
    // was seen exactly one frame ago, and a racing game breaks both: frame times vary, and a glow
    // mesh can miss a frame to an LOD change or a cull. At speed a car covers a metre or more per
    // frame, so either leaves a residual past the 0.9 m radius. The lamp is then not recognised, it
    // gets a fresh slot - a new Remix light - and the slot it left behind lives out its fade at its
    // last position: a light hanging back from the car, and only when it is going fast. That was the
    // "lights fall behind when you speed up".
    //
    // A lamp's position in its owner's own space never moves: a tail light is at the same point on
    // the car in every frame. So when both sides carry one, a slot is only a candidate if it is the
    // same model-space point (kLocalMatchDist - lamps of one car sit further apart than that, and
    // lamps closer than that use different glow sheets), and once that holds, the world-space radius
    // can grow with the lamp's speed without the risk it used to carry: it can no longer make two
    // lamps of the same car trade places. What it can still confuse is two cars of the same model
    // with the same lamp inside the grown radius of each other, which is rare, and then only for
    // the frame they are that close. The radius is capped so a lamp cannot be claimed from across a
    // junction.
    constexpr f32 kLocalMatchDist = 0.2f;
    constexpr f32 kMaxTrackedMatchDist = 12.0f;

    // Kind switches are applied here, before a slot is claimed, so a disabled kind occupies neither
    // a pool entry nor a cell-grid bucket. The flare itself still draws - these settings control
    // what a glow EMITS, not whether it is visible, which is the distinction the original engine
    // draws too (every one of these was a pure billboard that lit nothing).
    if (!agiGlowKindEnabled(agiClassifyGlowKind(texture ? texture->Tex.Name : nullptr, tint)))
        return;

    f32 best_dist_sq = kMaxTrackedMatchDist * kMaxTrackedMatchDist;
    agiGlowLight* slot = nullptr;
    bool fresh = false;

    for (u32 i = 0; i < agiGlowLightCount; ++i)
    {
        agiGlowLight& candidate = agiGlowLights[i];

        if ((candidate.Texture != texture) || (candidate.Age == 0))
            continue;

        // Frames since the slot was last refreshed: agiUpdateGlowLights ages every slot at the start
        // of the frame, so one seen last frame reads 1 here. Velocity is per frame, so a lamp that
        // missed a frame has moved twice as far as one step predicts.
        const f32 frames = static_cast<f32>(candidate.Age);
        const Vector3 predicted = candidate.Position + (candidate.Velocity * frames);
        const f32 dist_sq = (predicted - position).Mag2();

        f32 limit_sq = kMatchDistSq;

        if (local && candidate.HasLocal)
        {
            if ((candidate.Local - *local).Mag2() > (kLocalMatchDist * kLocalMatchDist))
                continue;

            // Slack in proportion to how far the lamp travels in the time since it was seen - a frame
            // taking half as long again as the last one leaves half a step of residual.
            const f32 step = std::sqrt(candidate.Velocity.Mag2()) * frames;
            const f32 limit = std::min(kMatchDist + step, kMaxTrackedMatchDist);
            limit_sq = limit * limit;
        }
        else if (local || candidate.HasLocal)
        {
            // One route knows its model-space position and the other does not: not the same flare.
            continue;
        }

        if ((dist_sq < limit_sq) && (dist_sq < best_dist_sq))
        {
            best_dist_sq = dist_sq;
            slot = &candidate;
        }
    }

    if (!slot)
    {
        // Not a sprite already being tracked, so this is a new light - including when it takes over
        // an evicted slot below, which held a different light a moment ago.
        fresh = true;

        if (agiGlowLightCount < AGI_MAX_GLOW_LIGHTS)
        {
            slot = &agiGlowLights[agiGlowLightCount++];
        }
        else
        {
            // Full: evict the stalest entry rather than dropping this one. A slot that has not been
            // seen for longest is the one least likely to still matter.
            u32 oldest = 0;

            for (u32 i = 1; i < agiGlowLightCount; ++i)
            {
                if (agiGlowLights[i].Age > agiGlowLights[oldest].Age)
                    oldest = i;
            }

            slot = &agiGlowLights[oldest];
        }
    }

    // Track velocity from the actual step taken, so the next frame's prediction stays accurate.
    // A newly allocated slot has no history and starts at rest; it gets a real velocity from its
    // second frame onwards, which is soon enough - one frame of lag on a light's first appearance
    // is not observable.
    //
    // Keyed on `fresh`, not on the slot's texture matching: an evicted slot can hold a different
    // light drawn with the same sheet, and its old position is no history of this one.
    //
    // Per frame, averaged over the frames since the slot was last seen (its Age, not yet reset).
    slot->Velocity = fresh ? Vector3 {0.0f, 0.0f, 0.0f}
                           : ((position - slot->Position) * (1.0f / static_cast<f32>(std::max<u32>(slot->Age, 1))));

    slot->Position = position;
    slot->Tint = tint;
    // Provisional classification, from the tint alone. The renderer refines it once it has resolved
    // the flare's actual hue out of the glow texture - see agiClassifyGlowIntensity.
    slot->Intensity = agiClassifyGlowIntensity(texture ? texture->Tex.Name : nullptr, tint);
    slot->Radius = std::max(radius, 1.0f);
    slot->Texture = texture;
    slot->U = u;
    slot->V = v;
    slot->Age = 0;
    slot->Direction = direction;
    slot->ConeAngle = cone_angle;
    slot->Local = local ? *local : Vector3 {0.0f, 0.0f, 0.0f};
    slot->HasLocal = local ? 1u : 0u;

    if (fresh)
    {
        // 0 marks a free slot, so skip it on wrap-around.
        if (++s_next_glow_light_id == 0)
            ++s_next_glow_light_id;

        slot->Id = s_next_glow_light_id;
    }

    if (PARAM_glowdebug.get_or(false) && texture)
    {
        // Deduplicate on texture AND coarse hue, not on the texture alone.
        //
        // Keying on the agiTexDef pointer hid the single most important case. One FXLTGLOW sheet
        // carries both the traffic signals and the street lamps; whichever was drawn first claimed
        // the slot, so if a green signal came up before any lamp - which it does - no street lamp
        // ever printed a line, and the log looked exactly as it would if lamps were never harvested
        // at all. That is not a small logging nicety: it is the difference between "lamps are not in
        // the draw stream" and "lamps are in the draw stream and something later drops them", which
        // are opposite bugs with opposite fixes.
        struct SeenGlow
        {
            const agiTexDef* Texture;
            u8 R, G, B;
        };

        static SeenGlow seen[128] {};
        static u32 seen_count = 0;

        // Quantise the hue to eighths, so the same lamp at different brightnesses collapses to one
        // entry while genuinely different colours on one sheet stay apart.
        const f32 peak = std::max({tint.x, tint.y, tint.z, 1e-4f});

        const SeenGlow key {texture, static_cast<u8>(tint.x / peak * 8.0f), static_cast<u8>(tint.y / peak * 8.0f),
            static_cast<u8>(tint.z / peak * 8.0f)};

        bool known = false;

        for (u32 i = 0; i < seen_count; ++i)
        {
            known |= (seen[i].Texture == key.Texture) && (seen[i].R == key.R) && (seen[i].G == key.G) &&
                (seen[i].B == key.B);
        }

        if (!known && (seen_count < 128))
        {
            seen[seen_count++] = key;

            const f32 saturation = (peak - std::min({tint.x, tint.y, tint.z})) / peak;

            Displayf("GLOW: tex='%s' props=0x%X radius=%.1f tint=(%.2f %.2f %.2f) sat=%.2f intensity=%.2f "
                     "uv=(%.2f %.2f) pos=(%.1f %.1f %.1f)",
                texture->Tex.Name, texture->Tex.Props, slot->Radius, tint.x, tint.y, tint.z, saturation,
                slot->Intensity, u, v, position.x, position.y, position.z);
        }
    }
}

void agiResetGlowLights()
{
    // Zero the slots as well as the count. The count alone would be enough for the loops that read
    // this registry, but leaving freed agiTexDef pointers lying in the array is exactly the state
    // that produced the crash this exists to prevent - so do not leave them readable.
    for (u32 i = 0; i < agiGlowLightCount; ++i)
        agiGlowLights[i] = {};

    agiGlowLightCount = 0;
}

void agiUpdateGlowLights()
{
    // Age every slot, then compact out the expired ones. Slots that were refreshed this frame go
    // back to age 0 in agiAddGlowLightRGB; everything else drifts towards expiry and fades on the
    // way, so a light that genuinely goes away leaves smoothly instead of popping.
    u32 live = 0;

    for (u32 i = 0; i < agiGlowLightCount; ++i)
    {
        agiGlowLight& light = agiGlowLights[i];

        if (++light.Age >= AGI_GLOW_LIGHT_TTL)
            continue;

        agiGlowLights[live++] = light;
    }

    agiGlowLightCount = live;

    agiGlowCardsSeen = 0;
    agiGlowCardsNoTexture = 0;
    agiGlowCardsNotGlow = 0;
    agiGlowCardsHarvested = 0;
}

void agiAddGlowLight(
    const Vector3& position, u32 color, f32 scale, agiTexDef* texture, f32 u, f32 v, const Vector3* local)
{
    // The billboard's own alpha is its current brightness - glows fade with distance and with
    // whatever the caller is animating (traffic lights cycling, headlight glow with the beam).
    // Folding it in here means the emitted light fades with the sprite instead of popping.
    f32 intensity = static_cast<f32>((color >> 24) & 0xFF) / 255.0f;

    if (intensity <= 0.0f)
        return;

    Vector3 rgb {
        (static_cast<f32>((color >> 16) & 0xFF) / 255.0f) * intensity,
        (static_cast<f32>((color >> 8) & 0xFF) / 255.0f) * intensity,
        (static_cast<f32>(color & 0xFF) / 255.0f) * intensity,
    };

    // The billboard's radius is the size of the *visible flare*, not the reach of the light it
    // stands for - a street lamp's corona is a metre or so across while it lights several metres of
    // pavement. agiGlowLightReach() makes that conversion, and is shared with the glow-mesh route so
    // the same fixture gets the same reach whichever way the engine happens to draw it.
    agiAddGlowLightRGB(position, rgb, agiGlowLightReach(scale), texture, u, v, Vector3 {0.0f, 0.0f, 0.0f}, 0.0f, local);
}

void agiMeshSet::DrawCard(Vector3& position, f32 scale, u32 rotation, u32 color, u32 frame)
{
    const agiViewParameters& view_params = ViewParams();
    const Matrix34& matrix = view_params.ModelView;

    f32 w = -(matrix.m0.z * position.x + matrix.m1.z * position.y + matrix.m2.z * position.z + matrix.m3.z);
    f32 z = w * ProjZZ + ProjZW;

    if (-w > z || z > w)
        return;

    // Harvest this billboard as a light source if it is an AlphaGlow - see agiworld/glowlight.h.
    // Placed after the depth reject above so off-screen-in-depth glows are not recorded, but before
    // the screen-space clipping below, which would otherwise drop lights whose flare is just off
    // the edge of the view while their illumination still falls on visible geometry.
    ++agiGlowCardsSeen;

    agiTexDef* card_texture = agiCurState.GetTexture();

    if (!card_texture)
    {
        ++agiGlowCardsNoTexture;

        // Where are the untextured cards, in world space?
        //
        // This is the one measurement that separates two opposite conclusions. A card with no
        // agiCurState texture is dropped by the harvest below, and there are 40-odd of them per
        // frame in a dense night scene. If they sit at lamp-head height they ARE the street lamps
        // and the harvest point is wrong; if they sit on the ground they are blob shadows, which
        // are legitimately untextured and mean the lamps are failing somewhere else entirely.
        // Guessing between those has already cost one wrong diagnosis.
        if (PARAM_glowdebug.get_or(false))
        {
            static u32 logged = 0;

            if (logged < 24)
            {
                ++logged;
                Displayf("CARD-NOTEX: pos=(%.1f %.1f %.1f) scale=%.3f color=%08X frame=%u", position.x, position.y,
                    position.z, scale, color, frame);
            }
        }
    }
    else if (!(card_texture->Tex.Props & agiTexProp::AlphaGlow))
    {
        ++agiGlowCardsNotGlow;

        // Same question for the textured-but-not-AlphaGlow population: a street lamp whose flare
        // sheet simply is not flagged AlphaGlow would land here and be just as invisible.
        if (PARAM_glowdebug.get_or(false))
        {
            static const agiTexDef* seen[32] {};
            static u32 seen_count = 0;

            bool known = false;

            for (u32 i = 0; i < seen_count; ++i)
                known |= (seen[i] == card_texture);

            if (!known && (seen_count < 32))
            {
                seen[seen_count++] = card_texture;
                Displayf("CARD-NOTGLOW: tex='%s' props=0x%X pos=(%.1f %.1f %.1f) scale=%.3f", card_texture->Tex.Name,
                    card_texture->Tex.Props, position.x, position.y, position.z, scale);
            }
        }
    }

    // The billboard route of the glow harvest: street lamps, traffic signals, lit signage. Restored
    // from before Pathway B was unwired, for the Remix API (agidx9/dx9remix.cpp), and off until that
    // connects - see agiGlowHarvestEnabled.
    //
    // The CardsSeen / CardsNoTexture / CardsNotGlow counters above run either way. They describe the
    // draw stream rather than any consumer, and they answer a question nothing visual can: whether a
    // glow reaches this point at all.
    if (agiGlowHarvestEnabled && card_texture && (card_texture->Tex.Props & agiTexProp::AlphaGlow))
    {
        ++agiGlowCardsHarvested;

        // Sample where this frame actually reads. A traffic light is one texture holding red, amber
        // and green, selected by `frame` - see the note in agiworld/glowlight.h. Getting this wrong
        // gives every signal the same muddy colour in all three states.
        const Vector2* frame_uvs = &CurrentMeshCard.Frames[4 * frame];

        f32 u = 0.0f;
        f32 v = 0.0f;

        for (i32 k = 0; k < 4; ++k)
        {
            u += frame_uvs[k].x;
            v += frame_uvs[k].y;
        }

        // `position` is in MODEL space, not world space. See docs/remix_api_data_sources.md §1.2.
        //
        // DrawCard projects through view_params.ModelView, which is View * World, so whatever world
        // matrix is current applies to this position. asParticles::Cull() sets the world to
        // IDENTITY before its cards, so for particles, smoke and vehicle glows model space happens
        // to be world space. mmBangerInstance::DrawGlow() does not: it sets the banger's own
        // transform, so the position it passes is the banger-local glow offset. Taking it raw put
        // every street lamp in the city at its mmBangerData::GlowOffset - within a couple of metres
        // of the world origin, merged into a single light there.
        //
        // Transforming by ViewParams().World is right for both: it is the matrix DrawCard is already
        // using, and it is a no-op for the identity case.
        //
        // Hand tuning from Open1560_RemixAPI.ini (agiworld/glowtune.h) applies here, while the
        // position is still local: an offset then follows the banger's own transform, and "outward"
        // can be judged against the local X - for a street lamp, away from its pole. The kind is
        // the harvest-time one, from the card's tint; a per-texture section avoids relying on it.
        const Vector3 tint {static_cast<f32>((color >> 16) & 0xFF), static_cast<f32>((color >> 8) & 0xFF),
            static_cast<f32>(color & 0xFF)};

        const agiGlowTuning tuning =
            agiResolveGlowTuning(card_texture->Tex.Name, agiClassifyGlowKind(card_texture->Tex.Name, tint));

        if (!tuning.HasEnabled || tuning.Enabled)
        {
            Vector3 world_position;
            world_position.Dot(position + agiGlowLocalOffset(tuning, position), view_params.World);

            // The card's own position is its model-space identity - see agiAddGlowLightRGB.
            agiAddGlowLight(world_position, color, scale, card_texture, u * 0.25f, v * 0.25f, &position);
        }
    }

    f32 x = matrix.m0.x * position.x + matrix.m1.x * position.y + matrix.m2.x * position.z + matrix.m3.x;
    f32 y = matrix.m0.y * position.x + matrix.m1.y * position.y + matrix.m2.y * position.z + matrix.m3.y;

    u8 clip_any = 0;
    u8 clip_all = 0xFF;
    PodArray<Vector2, 4> positions;

    Vector2* rotations = &CurrentMeshCard.Rotations[4 * ((CurrentMeshCard.PointCount - 1) & rotation)];
    i32 vert_count = CurrentMeshCard.VertCount;

    for (i32 i = 0; i < vert_count; ++i)
    {
        f32 vert_x = (scale * rotations[i].x + x) * view_params.ProjX + z * view_params.ProjXZ;
        f32 vert_y = (scale * rotations[i].y + y) * view_params.ProjY + z * view_params.ProjYZ;

        i32 clip = 0;
        clip |= (-w > vert_x) ? MESH_CLIP_NX : (vert_x > w) ? MESH_CLIP_PX : 0;
        clip |= (-w > vert_y) ? MESH_CLIP_NY : (vert_y > w) ? MESH_CLIP_PY : 0;

        clip_any |= clip;
        clip_all &= clip;
        positions[i].x = vert_x;
        positions[i].y = vert_y;
    }

    if (clip_all)
        return;

    if (agiCurState.GetDrawMode() == agiDrawDepth)
        color = 0xFF202020;

    u32 fog = CalculateFog(w, FogValue) << 24;
    f32 inv_w = 1.0f / w;

    if (f32 size = scale * inv_w; size < MinCardSize || size > MaxCardSize)
        return;

    f32 vert_z = z * inv_w * DepthScale + DepthOffset;
    Vector2* tex_coords = &CurrentMeshCard.Frames[4 * frame];

    // World-space billboard. Everything above this point still runs: the depth reject, the
    // screen-size gate and the clip_all test are all cheap scalar work on four corners, and keeping
    // them means this path culls exactly what the CPU path culled. What is skipped is the part that
    // destroys the world-space information - the perspective divide, the HalfWidth/OffsX viewport
    // map and ClampToScreen - along with the partial-clip branch below, because the GPU clips.
    //
    // The quad's corner offsets go out as the vertices, unmodified, and the card's position, size
    // and orientation go into the world matrix. See agiworld/cardworld.h for why that split is the
    // point of the exercise rather than an implementation detail.
    //
    // Camera.m0/m1 are the camera's X and Y axes in world space (View is Camera's inverse), so a
    // card-space offset along them lands on view-space X and Y - which is exactly where the CPU
    // path put it, adding `scale * rotations[i]` to the ModelView-transformed position.
    //
    // `position` is MODEL space. This is the trap documented above and it is invisible for half the
    // callers, because asParticles::Cull() sets SetWorld(IDENTITY) before its cards while
    // mmBangerInstance::DrawGlow() sets the banger's own transform. Transforming by
    // ViewParams().World is correct for both and collapses to a no-op for the first.
    if (NativePathEnabled(NATIVE_DRAWCARD) && agiWorldQuadsSupported())
    {
        Vector3 world_pos;
        world_pos.Dot(position, view_params.World);

        const Matrix34& camera = view_params.Camera;

        Matrix34 card_world {camera.m0 * scale, camera.m1 * scale, camera.m2, world_pos};

        agiQueueWorldQuad(agiCurState.GetTexture(), card_world, rotations, tex_coords, vert_count, color, w);
        return;
    }

    if (clip_any & ClipMask)
    {
        for (i32 i = 0; i < vert_count - 2; ++i)
        {
            PodArray<CV, 16> cv_in;
            PodArray<CV, 16> cv_out;
            PodArray<agiScreenVtx, 16> verts;

            cv_in[0] = CV(positions[0], z, w, 1.0f, 0.0f, 0.0f, 0, i + 1, i + 2);
            cv_in[1] = CV(positions[i + 1], z, w, 0.0f, 1.0f, 0.0f, 0, i + 1, i + 2);
            cv_in[2] = CV(positions[i + 2], z, w, 0.0f, 0.0f, 1.0f, 0, i + 1, i + 2);

            if (i32 clipped = FullClip(cv_out, cv_in, 3))
            {
                agiPolySet* poly = nullptr;

                if (!agiCurState.GetSoftwareRendering())
                    poly = agiTexSorter::BeginVerts(agiCurState.GetTexture(), clipped, 3 * (clipped - 2));

                for (i32 j = 0; j < clipped; ++j)
                {
                    agiScreenVtx& vert = !agiCurState.GetSoftwareRendering() ? poly->Vert() : verts[clipped - j - 1];

                    vert.x = cv_out[j].x * inv_w * HalfWidth + OffsX;
                    vert.y = cv_out[j].y * inv_w * HalfHeight + OffsY;
                    vert.z = vert_z;
                    vert.w = inv_w;

                    vert.tu = cv_out[j].map[0] * tex_coords[cv_out[j].idx[0]].x +
                        cv_out[j].map[1] * tex_coords[cv_out[j].idx[1]].x +
                        cv_out[j].map[2] * tex_coords[cv_out[j].idx[2]].x;

                    vert.tv = cv_out[j].map[0] * tex_coords[cv_out[j].idx[0]].y +
                        cv_out[j].map[1] * tex_coords[cv_out[j].idx[1]].y +
                        cv_out[j].map[2] * tex_coords[cv_out[j].idx[2]].y;

                    vert.color = color;
                    vert.specular = fog;

                    ClampToScreen(vert);
                }

                if (agiCurState.GetSoftwareRendering())
                {
                    swPoly(verts, clipped);
                }
                else
                {
                    for (i32 j = 2; j < clipped; ++j)
                        poly->Triangle(0, j, j - 1);

                    agiTexSorter::EndVerts();
                }
            }
        }
    }
    else
    {
        PodArray<agiScreenVtx, 4> verts;

        for (i32 i = 0; i < vert_count; ++i)
        {
            verts[i].x = positions[i].x * inv_w * HalfWidth + OffsX;
            verts[i].y = positions[i].y * inv_w * HalfHeight + OffsY;
            verts[i].z = vert_z;
            verts[i].w = inv_w;
            verts[i].tu = tex_coords[i].x;
            verts[i].tv = tex_coords[i].y;
            verts[i].color = color;
            verts[i].specular = fog;

            ClampToScreen(verts[i]);
        }

        if (agiCurState.GetSoftwareRendering())
        {
            if (vert_count == 3)
                swTri(&verts[2], &verts[1], &verts[0]);
            else
                swQuad(&verts[3], &verts[2], &verts[1], &verts[0]);
        }
        else
        {
            agiPolySet* poly = agiTexSorter::BeginVerts(agiCurState.GetTexture(), vert_count, 3 * (vert_count - 2));

            for (i32 i = 0; i < vert_count; ++i)
                poly->Vert() = verts[i];

            if (CurrentMeshCard.VertCount == 4)
                poly->Quad(3, 2, 1, 0);
            else
                poly->Triangle(2, 1, 0);

            agiTexSorter::EndVerts();
        }
    }
}

// Half-width, in world units, of a line submitted through the world-space path, and the fraction of
// its view depth added to that so a distant spark does not shrink below a pixel and disappear.
//
// A world-space line has to have a real thickness, where the screen-space one it replaces was always
// exactly one pixel however far away it was. Both numbers are cmd_params rather than constants
// because this is precisely the kind of thing that wants tuning against a running game, and a
// rebuild of this project is a CI round trip.
static mem::cmd_param PARAM_worldlinewidth {"worldlinewidth", "World-space half-width of spark and debug lines"};
static mem::cmd_param PARAM_worldlinegrow {"worldlinegrow", "Extra line half-width per unit of view depth"};

// Canonical line quad in "line space": x runs 0..1 along the segment, y is +/-0.5 across it, so the
// matrix carries the endpoints, the length, the thickness and the orientation. Every line in the
// game therefore submits the same four vertices and collapses to a single geometry hash, which is
// the same trick the billboards use - see agiworld/cardworld.h.
static const Vector2 kLineQuadCorners[4] {{0.0f, -0.5f}, {1.0f, -0.5f}, {1.0f, 0.5f}, {0.0f, 0.5f}};
static const Vector2 kLineQuadUVs[4] {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

void agiMeshSet::DrawLines(Vector3* starts, Vector3* ends, u32* colors, i32 count)
{
    agiMeshSet::Init(false);

    const bool world_path = NativePathEnabled(NATIVE_DRAWLINES) && agiWorldQuadsSupported();
    const agiViewParameters& view_params = ViewParams();

    const f32 line_width = PARAM_worldlinewidth.get_or(0.02f);
    const f32 line_grow = PARAM_worldlinegrow.get_or(0.0015f);

    agiScreenVtx* ARTS_RESTRICT verts = ARTS_ALLOCA(agiScreenVtx, count * 2);
    u32 stored = 0;

    for (; count; --count)
    {
        Vector3 start = *starts++;
        Vector3 end = *ends++;
        u32 color = *colors++;

        f32 start_w = M.m0.z * start.x + M.m1.z * start.y + M.m2.z * start.z + M.m3.z;
        f32 start_z = start_w * ProjZZ + ProjZW;

        if (-start_w > start_z || start_z > start_w)
            continue;

        f32 end_w = M.m0.z * end.x + M.m1.z * end.y + M.m2.z * end.z + M.m3.z;
        f32 end_z = end_w * ProjZZ + ProjZW;

        if (-end_w > end_z || end_z > end_w)
            continue;

        f32 start_x = M.m0.x * start.x + M.m1.x * start.y + M.m2.x * start.z + M.m3.x;
        f32 start_y = M.m0.y * start.x + M.m1.y * start.y + M.m2.y * start.z + M.m3.y;

        if (-start_w > start_x || start_x > start_w || -start_w > start_y || start_y > start_w)
            continue;

        f32 end_x = M.m0.x * end.x + M.m1.x * end.y + M.m2.x * end.z + M.m3.x;
        f32 end_y = M.m0.y * end.x + M.m1.y * end.y + M.m2.y * end.z + M.m3.y;

        if (-end_w > end_x || end_x > end_w || -end_w > end_y || end_y > end_w)
            continue;

        // World-space line, as a camera-facing quad. Same rejects above, same colour, but the
        // segment keeps its world position instead of being flattened onto the screen.
        //
        // `starts`/`ends` are model space - the rejects above run them through M, which is
        // model-to-view - so they go through ViewParams().World for the same reason DrawCard's
        // position does.
        if (world_path)
        {
            Vector3 world_start;
            Vector3 world_end;
            world_start.Dot(start, view_params.World);
            world_end.Dot(end, view_params.World);

            Vector3 along = world_end - world_start;

            if (along.Mag2() < 1e-8f)
                continue;

            // Across the segment and facing the camera. Camera.m2 is the camera's Z axis in world
            // space; a segment pointing straight down it has no well-defined across direction, so
            // fall back to the camera's own X axis.
            Vector3 across;
            across.Cross(along, view_params.Camera.m2);

            if (f32 across_mag2 = across.Mag2(); across_mag2 > 1e-12f)
                across = across * (1.0f / std::sqrt(across_mag2));
            else
                across = view_params.Camera.m0;

            const f32 width = 2.0f * (line_width + line_grow * start_w);

            Matrix34 line_world {along, across * width, view_params.Camera.m2, world_start};

            agiQueueWorldQuad(nullptr, line_world, kLineQuadCorners, kLineQuadUVs, 4, color, start_w);
            continue;
        }

        f32 start_rhw = 1.0f / start_w;
        f32 end_rhw = 1.0f / end_w;

        verts[stored].x = start_x * start_rhw * HalfWidth + OffsX;
        verts[stored].y = start_y * start_rhw * HalfHeight + OffsY;
        verts[stored].z = start_z * start_rhw * DepthScale + DepthOffset;
        verts[stored].w = start_rhw;
        verts[stored].color = color;
        verts[stored].specular = 0xFF000000;
        verts[stored].tv = 0.0f;
        verts[stored].tu = 0.0f;
        ClampToScreen(verts[stored]);
        ++stored;

        verts[stored].x = end_x * end_rhw * HalfWidth + OffsX;
        verts[stored].y = end_y * end_rhw * HalfHeight + OffsY;
        verts[stored].z = end_z * end_rhw * DepthScale + DepthOffset;
        verts[stored].w = end_rhw;
        verts[stored].color = color;
        verts[stored].specular = 0xFF000000;
        verts[stored].tv = 0.0f;
        verts[stored].tu = 0.0f;
        ClampToScreen(verts[stored]);
        ++stored;
    }

    if (stored)
    {
        ArAssert((stored & 1) == 0, "Invalid Vertex Count");

        RAST->LineList(agiVtxType::Screen, (agiVtx*) verts, stored);
    }
}

// clang-format off
static u16 QuadIndices[144] {
     0,  1,  2,  0,  2,  3,
     4,  5,  6,  4,  6,  7,
     8,  9, 10,  8, 10, 11,
    12, 13, 14, 12, 14, 15,
    16, 17, 18, 16, 18, 19,
    20, 21, 22, 20, 22, 23,
    24, 25, 26, 24, 26, 27,
    28, 29, 30, 28, 30, 31,
    32, 33, 34, 32, 34, 35,
    36, 37, 38, 36, 38, 39,
    40, 41, 42, 40, 42, 43,
    44, 45, 46, 44, 46, 47,
    48, 49, 50, 48, 50, 51,
    52, 53, 54, 52, 54, 55,
    56, 57, 58, 56, 58, 59,
    60, 61, 62, 60, 62, 63,
    64, 65, 66, 64, 66, 67,
    68, 69, 70, 68, 70, 71,
    72, 73, 74, 72, 74, 75,
    76, 77, 78, 76, 78, 79,
    80, 81, 82, 80, 82, 83,
    84, 85, 86, 84, 86, 87,
    88, 89, 90, 88, 90, 91,
    92, 93, 94, 92, 94, 95,
};
// clang-format on

void agiMeshSet::DrawWideLines(Vector3* starts, Vector3* ends, f32* widths, u32* colors, i32 count)
{
    const agiViewParameters& vp = ViewParams();

    agiMeshSet::Init(false);

    agiScreenVtx* ARTS_RESTRICT verts = ARTS_ALLOCA(agiScreenVtx, count * 4);
    u32 specular = CalculateFog(M.m3.z, FogValue) << 24;

    u32 stored = 0;
    u32 num_indices = 0;

    for (; count; --count)
    {
        Vector3 start = *starts++;
        Vector3 end = *ends++;
        f32 start_width = *widths++;
        f32 end_width = *widths++;
        u32 color = *colors++;

        f32 start_w = M.m0.z * start.x + M.m1.z * start.y + M.m2.z * start.z + M.m3.z;
        f32 start_z = start_w * ProjZZ + ProjZW;

        if (-start_w > start_z || start_z > start_w)
            continue;

        f32 end_w = M.m0.z * end.x + M.m1.z * end.y + M.m2.z * end.z + M.m3.z;
        f32 end_z = end_w * ProjZZ + ProjZW;

        if (-end_w > end_z || end_z > end_w)
            continue;

        f32 start_x = M.m0.x * start.x + M.m1.x * start.y + M.m2.x * start.z + M.m3.x;
        f32 start_y = M.m0.y * start.x + M.m1.y * start.y + M.m2.y * start.z + M.m3.y;

        if (-start_w > start_x || start_x > start_w || -start_w > start_y || start_y > start_w)
            continue;

        f32 end_x = M.m0.x * end.x + M.m1.x * end.y + M.m2.x * end.z + M.m3.x;
        f32 end_y = M.m0.y * end.x + M.m1.y * end.y + M.m2.y * end.z + M.m3.y;

        if (-end_w > end_x || end_x > end_w || -end_w > end_y || end_y > end_w)
            continue;

        f32 start_rhw = 1.0f / start_w;
        f32 end_rhw = 1.0f / end_w;

        start_x = start_x * start_rhw * HalfWidth + OffsX;
        start_y = start_y * start_rhw * HalfHeight + OffsY;

        end_x = end_x * end_rhw * HalfWidth + OffsX;
        end_y = end_y * end_rhw * HalfHeight + OffsY;

        if (std::abs(start_y - end_y) >= std::abs(start_x - end_x))
        {
            start_width = (start_width * vp.ProjX + start_z * vp.ProjXZ) * start_rhw * HalfWidth;
            end_width = (end_width * vp.ProjX + end_z * vp.ProjXZ) * end_rhw * HalfWidth;

            verts[stored + 0].x = start_x + start_width;
            verts[stored + 0].y = start_y;

            verts[stored + 1].x = start_x - start_width;
            verts[stored + 1].y = start_y;

            verts[stored + 2].x = end_x - end_width;
            verts[stored + 2].y = end_y;

            verts[stored + 3].x = end_x + end_width;
            verts[stored + 3].y = end_y;
        }
        else
        {
            start_width = (start_width * vp.ProjY + start_z * vp.ProjYZ) * start_rhw * HalfHeight;
            end_width = (end_width * vp.ProjY + end_z * vp.ProjYZ) * end_rhw * HalfHeight;

            verts[stored + 0].x = start_x;
            verts[stored + 0].y = start_y - start_width;

            verts[stored + 1].x = start_x;
            verts[stored + 1].y = start_y + start_width;

            verts[stored + 2].x = end_x;
            verts[stored + 2].y = end_y + end_width;

            verts[stored + 3].x = end_x;
            verts[stored + 3].y = end_y - end_width;
        }

        start_z = start_z * start_rhw * DepthScale + DepthOffset;
        end_z = end_z * end_rhw * DepthScale + DepthOffset;

        start_z = std::max(std::min(start_z, 1.0f), 0.0f);
        end_z = std::max(std::min(end_z, 1.0f), 0.0f);

        verts[stored + 0].z = start_z;
        verts[stored + 0].w = start_rhw;
        verts[stored + 0].color = color;
        verts[stored + 0].specular = specular;
        verts[stored + 0].tu = 0.0f;
        verts[stored + 0].tv = 0.0f;

        verts[stored + 1].z = start_z;
        verts[stored + 1].w = start_rhw;
        verts[stored + 1].color = color;
        verts[stored + 1].specular = specular;
        verts[stored + 1].tu = 0.0f;
        verts[stored + 1].tv = 0.0f;

        verts[stored + 2].z = end_z;
        verts[stored + 2].w = end_rhw;
        verts[stored + 2].color = color;
        verts[stored + 2].specular = specular;
        verts[stored + 2].tu = 0.0f;
        verts[stored + 2].tv = 0.0f;

        verts[stored + 3].z = end_z;
        verts[stored + 3].w = end_rhw;
        verts[stored + 3].color = color;
        verts[stored + 3].specular = specular;
        verts[stored + 3].tu = 0.0f;
        verts[stored + 3].tv = 0.0f;

        for (i32 i = 0; i < 4; ++i)
        {
            verts[stored + i].x = std::max(std::min(verts[stored + i].x, MaxX), MinX);
            verts[stored + i].y = std::max(std::min(verts[stored + i].y, MaxY), MinY);
        }

        stored += 4;
        num_indices += 6;
    }

    if (stored)
    {
        ArAssert(num_indices <= ARTS_SIZE32(QuadIndices), "Too Many Indices");

        agiCullMode old_cull = agiCurState.SetCullMode(agiCullMode::None);
        agiCurState.SetTexture(nullptr);

        RAST->Mesh(agiVtxType::Screen, (agiVtx*) verts, stored, QuadIndices, num_indices);

        agiCurState.SetCullMode(old_cull);
    }
}
