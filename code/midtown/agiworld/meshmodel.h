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
#include "skeleton.h"

class agiMeshModel;
class bnAnimation;

class agiLitAnimation
{
public:
    // ??0agiLitAnimation@@QAE@PAVagiMeshModel@@PAVbnAnimation@@PAVStream@@@Z
    ARTS_IMPORT agiLitAnimation(agiMeshModel* arg1, bnAnimation* arg2, Stream* arg3);

    // Straight out of the constructor (game.asm ~339488): it writes the model to +0x00 and the
    // animation to +0x04, then allocates FrameCount pointers at +0x08 and, behind them, one block
    // of `FrameCount * model->AdjunctCount` BYTES which it Stream::Read()s in whole.
    //
    // One byte per adjunct per frame is exactly the shape of agiMeshSet::Normals - a packed index
    // into the 198-entry UnpackNormal[] table - which is what agiMeshModel::ModelDrawLit relies on:
    // it swaps this frame's array into Normals for the duration of the lighting call.
    offset_field(0x00, agiMeshModel*, Model);
    offset_field(0x04, bnAnimation*, Anim);
    offset_field(0x08, u8**, Frames);

    u8 gap0[0x10];
};

check_size(agiLitAnimation, 0x10);

class agiMeshModel final : public agiMeshSet
{
public:
    // ?ModelDraw@agiMeshModel@@QAEHIPAVagiLitAnimation@@H@Z
    ARTS_IMPORT i32 ModelDraw(u32 arg1, agiLitAnimation* arg2, i32 arg3);

    // ?ModelDraw@agiMeshModel@@QAEHIPAVbnAnimation@@H@Z
    ARTS_IMPORT i32 ModelDraw(u32 arg1, bnAnimation* arg2, i32 arg3);

    // Reimplemented in agiworld/meshmodel.cpp to add the world-space path for pedestrians. The
    // signature is spelled with agiMeshLighter rather than the raw function-pointer type it was
    // written as - they are the same type (meshset.h), so the mangled name the assembly calls is
    // unchanged.

    // ?ModelDrawLit@agiMeshModel@@QAEHP6AXPAEPAI1PAVagiMeshSet@@@ZIPAVagiLitAnimation@@H@Z
    i32 ModelDrawLit(agiMeshLighter arg1, u32 arg2, agiLitAnimation* arg3, i32 arg4);

    // ?ModelDrawSkel@agiMeshModel@@QAEHIPAVbnAnimation@@H@Z
    ARTS_IMPORT i32 ModelDrawSkel(u32 arg1, bnAnimation* arg2, i32 arg3);

    // ?ModelGeometry@agiMeshModel@@QAEHIPAVbnAnimation@@H@Z
    ARTS_IMPORT i32 ModelGeometry(u32 arg1, bnAnimation* arg2, i32 arg3);

    // The skinning inputs, read from agiMeshModel::ModelGeometry (game.asm ~339065). It walks
    // SkinGroupCount groups, taking SkinGroupVerts[g] vertices from agiMeshSet::Vertices in order
    // and transforming each by Skeleton.BoneMatrices[g] - so the mesh's vertex array is partitioned
    // into runs, one run rigidly bound per bone. There is no per-vertex weighting.
    //
    // The skeleton is embedded rather than pointed to: ModelGeometry does `lea ecx, [this+0x7C]`
    // and calls bnSkeleton::Pose on it directly. It is 0x20 bytes (check_size in skeleton.h), so it
    // spans 0x7C..0x9C - which is what identifies the two fields the assembly reads as [this+0x90]
    // and [this+0x98] as Skeleton.BoneCount and Skeleton.BoneMatrices rather than fields of this
    // class.
    offset_field(0x6C, i32, SkinGroupCount);
    offset_field(0x70, u8*, SkinGroupVerts);
    offset_field(0x7C, bnSkeleton, Skeleton);

    // Per-variation base colours. ModelDrawLit picks VariantColors[MESH_DRAW_GET_VARIANT(flags)]
    // when VariantColors is non-null and falls back to agiMeshSet::Colors otherwise - the `flags >> 4`
    // in the assembly is the same variant encoding meshset.h spells as MESH_DRAW_GET_VARIANT, which
    // is what identifies these two.
    offset_field(0x9C, u32, HasVariantColors);
    offset_field(0xA0, u32**, VariantColors);

    u8 gap64[0x44];
};

check_size(agiMeshModel, 0xA8);
