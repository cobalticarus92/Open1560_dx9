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

#include "meshset.h"

#include "vector7/vector4.h"

// ?SetClipMode@@YAXH@Z | unused
void SetClipMode(b32 mask_only_z);

// ?MaxCardSize@@3MA
ARTS_IMPORT extern f32 MaxCardSize;

// ?MinCardSize@@3MA
ARTS_IMPORT extern f32 MinCardSize;

// ?ShadowColor@@3IA
ARTS_IMPORT extern u32 ShadowColor;

// ?ShadowFudge@@3MA
ARTS_IMPORT extern f32 ShadowFudge;

// ?SphMapColor@@3IA
ARTS_IMPORT extern u32 SphMapColor;

// Normal coverage census, accumulated by agiMeshSet::DrawNativeTransform and reported by the DX9
// per-frame census. Answers a question that cannot be settled by reading call sites, because the
// decision is made per instance at load time and most of the instance code is still closed:
// how much of the frame actually carries per-vertex normals?
//
// A mesh loaded without MESH_SET_NORMAL has no normal array at all - mmInstance::InitMeshes only
// asks for one when the instance is a collider, a mover or an obstacle, and every other instance
// gets MESH_SET_UV | MESH_SET_NO_BOUND. (Strictly, a mesh has normals when its baked .bms does - the
// requested flags only matter when building from a .dlp source.) Those draws have hardware lighting
// disabled, which is faithful to what the CPU path did with them; the normals they submit are
// rebuilt from the geometry by default (-geonormals), because RTX Remix shades from them regardless.
extern u32 agiMeshNormalDraws;
extern u32 agiMeshNormalDrawsFlat;
extern u32 agiMeshNormalTris;
extern u32 agiMeshNormalTrisFlat;

// -geonormals: meshes whose normals were rebuilt from geometry, how many of those disagreed with the
// winding convention against their own stored normals (should stay ~0), and how many were too large
// to rebuild. Counted per build, so with the mesh cache on a mesh counts once, not once per frame.
// Not reset by agiResetMeshNormalStats: the census clears them after each report instead.
extern u32 agiMeshGeoNormalBuilds;
extern u32 agiMeshGeoNormalFlips;
extern u32 agiMeshGeoNormalSkipped;

void agiResetMeshNormalStats();

// The city's shared vehicle sphere map, pushed down by mmCullCity::Cull() once per frame. Null
// before a city is loaded, and null in the menus. See its definition for why mmcity pushes rather
// than agiworld pulling.
class agiTexDef;
extern agiTexDef* agiNativeCitySphereMap;

// Reflection census. "Offered" draws are those that reached the hardware path with a sphere map
// selected; the skip count says how many of those were refused for having no vertex normals. Both
// zero means nothing is asking for chrome, which is a different problem from chrome being refused.
extern u32 agiReflectDraws;
extern u32 agiReflectSkipNoNormals;

void agiResetReflectStats();

struct agiMeshCardVertex
{
    f32 x, y;
    f32 tu, tv;
};

check_size(agiMeshCardVertex, 0x10);

struct agiMeshCardInfo
{
public:
    // ?Init@agiMeshCardInfo@@QAEXHPAUagiMeshCardVertex@@HHH@Z
    ARTS_IMPORT void Init(
        i32 vertex_count, agiMeshCardVertex* verts, i32 rotation_count, i32 frames_width, i32 frames_height);

    i32 VertCount {};
    i32 PointCount {};
    Vector2* Rotations {};
    Vector2* Frames {};
};

check_size(agiMeshCardInfo, 0x10);

template <typename T>
inline void agiMeshSet::ClampToScreen(T& vert)
{
    vert.x = std::min<f32>(vert.x, MaxX);
    vert.y = std::min<f32>(vert.y, MaxY);
    vert.z = std::min<f32>(vert.z, 1.0f);

    vert.x = std::max<f32>(vert.x, MinX);
    vert.y = std::max<f32>(vert.y, MinY);
    vert.z = std::max<f32>(vert.z, 0.0f);
}

void agiBlendColors(u32* ARTS_RESTRICT shaded, u32* ARTS_RESTRICT colors, i32 count, u32 color);
