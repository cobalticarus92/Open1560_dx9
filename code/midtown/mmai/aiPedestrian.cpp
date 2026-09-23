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

define_dummy_symbol(mmai_aiPedestrian);

#include "aiPedestrian.h"

#include "agi/pipeline.h"
#include "agi/viewport.h"
#include "agiworld/meshmodel.h"
#include "agiworld/texsort.h"
#include "mmcity/cullcity.h"
#include "mmcity/inst.h"
#include "vector7/matrix34.h"

#ifdef ARTS_DEV_BUILD
void aiPedestrianInstance::AddWidgets(Bank* /*arg1*/)
{}
#endif

// agiworld/meshrend.cpp - the OPEN1560_NATIVE_MASK gate. 0x10 is NATIVE_DRAWMODEL, the pedestrian
// path in agiMeshModel::ModelDrawLit, so this bisects with it.
bool agiNativePathEnabled(u32 which);

// -pedsticks. Brings back the original's distant pedestrians, which are not meshes at all.
static mem::cmd_param PARAM_ped_sticks {
    "pedsticks", "Draw distant pedestrians as the original stick figures (invisible to RTX Remix)"};

// ?Draw@aiPedestrianInstance@@UAIXH@Z
//
// A straight lift of game.asm ~94669, except for which pedestrians get the real mesh.
//
// The original only draws the full, lit, skinned model at LOD 3. For every other LOD it takes
// agiMeshModel::ModelDrawSkel - unless the BoneScale debug tweak is above 1.5, which it never is
// (default 1.2). ModelDrawSkel does not draw the mesh: it poses the skeleton and hands one wide
// line per bone to agiMeshSet::DrawWideLines, which projects them on the CPU into flat,
// camera-facing screen quads. Every pedestrian past the nearest band was a stick figure made of
// screen-space ribbons, which is why RTX Remix saw only the few closest pedestrians: the rest
// were never 3D geometry at all.
//
// On a device that takes world-space geometry, every LOD now draws the real model through
// ModelDrawLit, which skins it on the GPU and submits a stable, model-space mesh - the same path
// the nearest pedestrians already take. That costs a few hundred triangles per distant pedestrian
// where a stick figure cost a dozen, which is nothing on any GPU the Remix bridge runs on.
//
// -pedsticks, a non-world-space renderer, or an OPEN1560_NATIVE_MASK without 0x10 restores the
// original choice exactly.
//
// The layout is read at the offsets the assembly uses, because gap14 is all this class declares:
//   +0x14  object holding the pedestrian's matrix at +0x3C
//   +0x18  animation set: +0x04 array of 0x28-byte entries, +0x0C the stick-figure model
//            entry +0x14 agiLitAnimation*, +0x18 bnAnimation*, +0x1C first frame
//   +0x1C  i16 frame within the current animation
//   +0x1E  u8 index of the current animation
//   +0x20  variant, which becomes MESH_DRAW_VARIANT via << 4
//   +0x24  written with lod + 1 on the way out
void aiPedestrianInstance::Draw(i32 lod)
{
    u8* const self = reinterpret_cast<u8*>(this);

    const i32 tri_count_before = agiPolySet::TriCount;

    const u8* const object = *reinterpret_cast<u8**>(self + 0x14);
    Matrix34 world = *reinterpret_cast<const Matrix34*>(object + 0x3C);
    world.Scale(BoneScale);

    Viewport()->SetWorld(world);

    const u8* const anim_set = *reinterpret_cast<u8**>(self + 0x18);
    const u8* const entry = *reinterpret_cast<u8* const*>(anim_set + 0x04) + (self[0x1E] * 0x28);

    const i32 frame = *reinterpret_cast<const i32*>(entry + 0x1C) + *reinterpret_cast<const i16*>(self + 0x1C);
    const u32 flags = (*reinterpret_cast<const u32*>(self + 0x20) << 4) | 1;

    agiLitAnimation* const lit_anim = *reinterpret_cast<agiLitAnimation* const*>(entry + 0x14);

    const bool original_full_mesh = (lod == 3) || (BoneScale > 1.5f);

    const bool world_full_mesh = !PARAM_ped_sticks.get_or(false) && Pipe()->SupportsNativeTransform() &&
        agiNativePathEnabled(0x10) && lit_anim && lit_anim->Model;

    if (original_full_mesh || world_full_mesh)
    {
        lit_anim->Model->ModelDrawLit(mmInstance::DynamicLighter, flags, lit_anim, frame);
    }
    else
    {
        agiMeshModel* const stick_model = *reinterpret_cast<agiMeshModel* const*>(anim_set + 0x0C);
        bnAnimation* const anim = *reinterpret_cast<bnAnimation* const*>(entry + 0x18);

        stick_model->ModelDrawSkel(flags, anim, frame);
    }

    *reinterpret_cast<i32*>(self + 0x24) = lod + 1;

    pedTriCount += agiPolySet::TriCount - tri_count_before;
}
