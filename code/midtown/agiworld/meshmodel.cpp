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

define_dummy_symbol(agiworld_meshmodel);

#include "meshmodel.h"

#include "agi/pipeline.h"
#include "agi/rsys.h"
#include "agi/viewport.h"
#include "memory/alloca.h"
#include "skeleton.h"
#include "vector7/matrix34.h"

// agiworld/meshrend.cpp - the OPEN1560_NATIVE_MASK gate, shared so the model path can be bisected
// with the same switch as every other draw entry point.
bool agiNativePathEnabled(u32 which);

#define NATIVE_DRAWMODEL 0x10u

// Skins the model into `out` in MODEL space.
//
// This is agiMeshModel::ModelGeometry's transform step (game.asm ~339065) lifted out, and nothing
// more: pose the skeleton for this frame, then walk the vertex array in runs, transforming each run
// by its bone's matrix. The binding is rigid - one run of vertices per bone, no per-vertex weights -
// which is why a plain Vector3::Dot against a single Matrix34 is the whole of it.
//
// ModelGeometry itself then hands the result to agiMeshSet::Geometry, which projects and clips it
// on the CPU. That last step is exactly what has to be skipped to keep the geometry in world space,
// so the skinning is duplicated here rather than reusing the closed routine, and the closed routine
// is left untouched for the CPU fallback below.
// Whether the skinning inputs are self-consistent enough to run. Checked up front, every time,
// rather than assumed.
//
// The loop below writes `sum(SkinGroupVerts[0, SkinGroupCount))` vertices into a buffer sized
// VertexCount and reads that many from Vertices, and nothing in the format guarantees those agree -
// the assembly this was lifted from simply trusted them. A mismatch is not a wrong picture, it is a
// buffer overrun on a stack allocation, and its symptom appears somewhere else entirely and
// intermittently: the frame after, on a menu transition, in an unrelated draw. That is the most
// expensive class of bug to chase, so it is worth a few adds per pedestrian to make it impossible.
//
// Bones are checked too, because BoneMatrices is indexed by group and only holds
// Skeleton.BoneCount entries.
static bool CanSkinModel(agiMeshModel* model)
{
    const i32 groups = model->SkinGroupCount;
    const u8* run_lengths = model->SkinGroupVerts;

    if ((groups <= 0) || !run_lengths || !model->Vertices || !model->Skeleton.BoneMatrices)
        return false;

    if (groups > model->Skeleton.BoneCount)
        return false;

    u32 total = 0;

    for (i32 group = 0; group < groups; ++group)
        total += run_lengths[group];

    return total <= model->VertexCount;
}

static void SkinModelVertices(agiMeshModel* model, bnAnimation* anim, i32 frame, Vector3* ARTS_RESTRICT out)
{
    model->Skeleton.Pose(anim->Poses[frame]);
    model->Skeleton.Transform(nullptr);

    const Vector3* ARTS_RESTRICT src = model->Vertices;
    const Matrix34* bones = model->Skeleton.BoneMatrices;
    const u8* run_lengths = model->SkinGroupVerts;

    for (i32 group = 0; group < model->SkinGroupCount; ++group)
    {
        const Matrix34& bone = bones[group];

        for (u32 i = run_lengths[group]; i--;)
            (out++)->Dot(*src++, bone);
    }
}

// -pedskin. Restores CPU skinning for pedestrians, which is what the world path did before the
// hardware-skinned submission below. Off by default: skinned positions change every frame, so every
// pedestrian took a fresh RTX Remix geometry hash every frame and no replacement could stick to one.
static mem::cmd_param PARAM_ped_skin {"pedskin", "Skin pedestrians on the CPU (breaks RTX Remix hash stability)"};

// Whether every facet lies wholly inside one bone's vertex run.
//
// Only consulted when a skeleton is too large for the device's matrix palette and has to be
// submitted in chunks. A chunk takes only the facets it owns outright, so a facet straddling the
// boundary between two chunks would not be drawn at all - a hole in the pedestrian rather than a
// visible seam. The models are built from rigid limb segments and in practice do not do this, but
// "in practice" is not something to bet a missing head on, so it is checked before committing and
// the CPU path is used unchanged when the check fails.
//
// A palette holding the whole skeleton has no boundary to straddle and does not need this at all,
// which is the usual case - the binding is per vertex there, so a facet with corners on two
// different bones simply draws, with each corner posed by its own.
//
// Cheap enough to run per draw: a pedestrian is a few hundred facets, and this is four range tests
// each against a run table that is already in cache.
static bool ModelFacetsAreGroupCoherent(agiMeshModel* model)
{
    const u8* run_lengths = model->SkinGroupVerts;
    const i32 groups = model->SkinGroupCount;

    for (u32 facet = 0; facet < model->SurfaceCount; ++facet)
    {
        const u16* surface = &model->SurfaceIndices[facet * 4];
        const u32 corners = surface[3] ? 4u : 3u;

        u32 first = 0;
        i32 facet_group = -1;

        for (i32 group = 0; group < groups; ++group)
        {
            const u32 end = first + run_lengths[group];

            const u32 vertex = model->VertexIndices[surface[0]];

            if ((vertex >= first) && (vertex < end))
            {
                facet_group = group;
                break;
            }

            first = end;
        }

        if (facet_group < 0)
            return false;

        const u32 end = first + run_lengths[facet_group];

        for (u32 k = 1; k < corners; ++k)
        {
            const u32 vertex = model->VertexIndices[surface[k]];

            if ((vertex < first) || (vertex >= end))
                return false;
        }
    }

    return true;
}

// ?ModelDrawLit@agiMeshModel@@QAEHP6AXPAEPAI1PAVagiMeshSet@@@ZIPAVagiLitAnimation@@H@Z
//
// Pedestrians. This is the one draw entry point the world-space work never reached, because peds do
// not go through agiMeshSet::DrawLit at all - aiPedestrianInstance::Draw (game.asm ~94669) calls
// here instead, and this used to end unconditionally in FirstPass(), i.e. CPU-pretransformed screen
// triangles. That is why pedestrians were lit by nothing: invisible to RTX Remix as 3D geometry, and
// never seen by the programmable path either, so they stayed flat while the wall behind them
// responded to every street lamp.
//
// The animation makes no difference to that. Once the vertices are skinned they are ordinary
// model-space positions, and the frame's normals are ordinary packed normal indices - the same two
// inputs DrawNativeTransform() already takes. So the fix is to skin, point the mesh at the result,
// and submit, exactly as agiMeshSet::DrawLit does for a static mesh.
i32 agiMeshModel::ModelDrawLit(agiMeshLighter lighter, u32 flags, agiLitAnimation* anim, i32 frame)
{
    // AGI_QUALITY_LOW: mmCullCity::fix_lighting() clears mmInstance::DynamicLighter outright at
    // that setting, so pedestrians arrive here with a null lighter. That used to send them straight
    // to ModelDraw(), the CPU path, whatever the native mask said - so at LOW lighting no pedestrian
    // reached RTX Remix at all. The engine is asking for them unlit, not for them in screen space:
    // the world-space paths below take them with hardware lighting off, which is exactly what
    // ModelDraw() shows, and only the CPU fallback at the bottom still needs ModelDraw() itself.
    const bool unlit = (lighter == nullptr);

    bnAnimation* animation = anim->Anim;

    // agiLitAnimation stores one packed normal per adjunct per frame, which is precisely the layout
    // of agiMeshSet::Normals - see the note on agiLitAnimation::Frames. Swapping it in for the
    // duration is what the original does for its lighter callback; the hardware path wants it for
    // the same reason, because it is the mesh's normal set for this frame.
    u8* saved_normals = Normals;
    u8* frame_normals = anim->Frames[frame];

    // The assembly tests +0x9C and only then indexes the +0xA0 table, so keep both: the flag is the
    // predicate, not the pointer.
    u32* base_colors = HasVariantColors ? VariantColors[MESH_DRAW_GET_VARIANT(flags)] : Colors;

    // Hardware skinning, on the fixed-function path, with a geometry hash that is a property of the
    // mesh rather than of the frame. This is the pedestrian path.
    //
    // The skeleton is posed on the CPU (it has to be - it is a handful of matrices, and the
    // animation data lives in system memory), and from there nothing else happens on the CPU at
    // all: the bone matrices go out through SetTransform(D3DTS_WORLDMATRIX(i)) and every vertex is
    // transformed by the vertex pipeline, exactly once, in the draw that submits it. Where the
    // device has no palette this degrades to one bone per draw against a plain world matrix, which
    // is the same arithmetic in the same place, just spread over more submissions.
    //
    // For RTX Remix, what matters is that NOTHING in the submitted vertex moves with the animation:
    //
    //  * positions are the mesh's stored bind-pose values, posed by the palette rather than by the
    //    CPU, which is what the earlier work on this path established;
    //  * colours and UVs are immutable mesh data and never were the problem;
    //  * normals are rebuilt from the mesh's own geometry (BuildSkinBindNormals). This is the piece
    //    that was missing. A pedestrian's normals otherwise come from agiLitAnimation's per-frame
    //    packed set, so the normal bytes changed on every frame of the walk cycle and Remix minted a
    //    new mesh for each - visible as the -ghashcolor tint strobing on pedestrians while
    //    everything else in the city held its colour.
    //
    // So a pedestrian now submits byte-identical vertices in every pose of every animation it plays,
    // and hashes to one mesh that a replacement can be attached to.
    //
    // The vertex-per-bone binding also removes the restriction the per-bone submission had: it drew
    // a facet with exactly one group, so a facet straddling two bones was drawn with neither and
    // left a hole, and the model had to be scanned for that in advance. A palette binds per VERTEX,
    // so a straddling facet simply draws with each corner on its own bone. The scan is only needed
    // when a skeleton is too large for the device's palette and has to be split across chunks.
    const u32 palette_limit = RAST ? RAST->MaxNativeSkinBones() : 0u;

    // BuildSkinBindNormals accumulates per vertex and emits per adjunct, both on the stack, and the
    // bound is the same one agiMeshSet::DrawNativeTransform applies to its own smoothing pass. A
    // pedestrian is far below it; anything above takes the CPU paths below.
    if (palette_limit && Pipe()->SupportsNativeTransform() && CanSkinModel(this) &&
        agiNativePathEnabled(NATIVE_DRAWMODEL) && frame_normals && !PARAM_ped_skin.get_or(false) &&
        (SkinGroupCount <= 255) && (VertexCount <= 4096) && (AdjunctCount <= 4096))
    {
        Skeleton.Pose(animation->Poses[frame]);
        Skeleton.Transform(nullptr);

        const agiViewParameters& view_params = ViewParams();
        const Matrix34* bones = Skeleton.BoneMatrices;
        const u8* run_lengths = SkinGroupVerts;

        const u32 group_count = static_cast<u32>(SkinGroupCount);

        // bone * world for each group - the palette itself - and the transpose of each bone's 3x3,
        // which is its inverse rotation. The transposes are not applied to any vertex: they exist so
        // that BuildSkinBindNormals can bring the animation's posed normals back into the mesh's own
        // space for the one sign comparison it makes. See agiNativeSkinPalette::NormalUnpose.
        Matrix34* palette = ARTS_ALLOCA(Matrix34, group_count);
        Matrix34* unpose = ARTS_ALLOCA(Matrix34, group_count);

        // Which bone owns each vertex. The mesh stores this as run LENGTHS, which is the wrong way
        // round for a per-vertex lookup, so it is expanded once here and then indexed.
        u8* vertex_bones = ARTS_ALLOCA(u8, VertexCount);

        u32 skinned_vertices = 0;

        for (u32 group = 0; group < group_count; ++group)
        {
            palette[group].Dot(bones[group], view_params.World);

            unpose[group].m0 = {bones[group].m0.x, bones[group].m1.x, bones[group].m2.x};
            unpose[group].m1 = {bones[group].m0.y, bones[group].m1.y, bones[group].m2.y};
            unpose[group].m2 = {bones[group].m0.z, bones[group].m1.z, bones[group].m2.z};
            unpose[group].m3 = {0.0f, 0.0f, 0.0f};

            for (u32 i = run_lengths[group]; i--;)
                vertex_bones[skinned_vertices++] = static_cast<u8>(group);
        }

        // Any vertices past the end of the last run belong to no bone - CanSkinModel establishes
        // only that the runs FIT in the array, not that they fill it. They stay outside every
        // chunk's range and so are never submitted, which is what the per-bone path does with them
        // too, and is why the ranges below are accumulated from the runs rather than set to
        // VertexCount.
        const u32 chunks = (group_count + palette_limit - 1) / palette_limit;

        // Only a skeleton that does not fit the device's palette needs this, and then only because
        // a chunk boundary reintroduces the per-bone path's problem: a facet spanning two chunks is
        // submitted with neither. A single chunk covers the whole vertex array, so nothing can span
        // anything and the scan is skipped - which is most of the point of this path.
        if ((chunks == 1) || ModelFacetsAreGroupCoherent(this))
        {
            // The animation's per-frame normal set, for the duration - restored below, exactly as
            // the paths beneath this one do.
            //
            // It is no longer what gets submitted: the vertices take their normals from the mesh's
            // geometry instead, which is what makes the hash stable. It is still needed for two
            // things. agiMeshSet::DrawNativeTransform tests Normals to decide whether the mesh can
            // be lit at all, and a pedestrian whose Normals are null submits unlit; and its non-null
            // contents are the reference BuildSkinBindNormals resolves the geometric sign against.
            Normals = frame_normals;

            b32 any_drawn = false;

            u32 first_bone = 0;
            u32 first_vertex = 0;

            while (first_bone < group_count)
            {
                const u32 end_bone = std::min(first_bone + palette_limit, group_count);

                u32 end_vertex = first_vertex;

                for (u32 group = first_bone; group < end_bone; ++group)
                    end_vertex += run_lengths[group];

                agiNativeSkinPalette skin {};

                skin.Bones = &palette[first_bone];
                skin.NormalUnpose = &unpose[first_bone];
                skin.Count = end_bone - first_bone;
                skin.VertexBones = vertex_bones;
                skin.FirstBone = first_bone;
                skin.FirstVertex = first_vertex;
                skin.EndVertex = end_vertex;

                // static_lighting = false for the same reason every other path here gives: a
                // pedestrian is a mover, lit by the dynamic rig.
                any_drawn |= DrawNativeTransform(flags, /*static_lighting=*/false, nullptr, base_colors, unlit, &skin);

                first_bone = end_bone;
                first_vertex = end_vertex;
            }

            Normals = saved_normals;

            if (any_drawn)
                return 1;

            // Nothing submitted means the mesh could not be expressed this way at all. Fall through
            // rather than dropping the pedestrian, as every path here does.
        }
    }

    if (Pipe()->SupportsNativeTransform() && frame_normals && CanSkinModel(this) &&
        agiNativePathEnabled(NATIVE_DRAWMODEL))
    {
        // ARTS_ALLOCA rather than a std::vector or the game's own allocator, both for the reason
        // documented in agiMeshSet::DrawNativeTransform - calling the engine allocator from inside a
        // draw corrupts the simulation heap - and because this is per-frame scratch with an obvious
        // lifetime. A pedestrian is a few hundred vertices; the ceiling is BigVtxSize.
        Vector3* skinned = ARTS_ALLOCA(Vector3, VertexCount);

        SkinModelVertices(this, animation, frame, skinned);

        Vector3* saved_vertices = Vertices;

        Vertices = skinned;
        Normals = frame_normals;

        // static_lighting = false: a pedestrian is a mover, lit by the dynamic rig, not by the
        // city's fixed sun/fill/fill. That is the same choice DrawLit() makes via
        // IsStaticCityLighter(), which never matches the ped lighter.
        const b32 drawn = DrawNativeTransform(flags, /*static_lighting=*/false, nullptr, base_colors, unlit);

        Vertices = saved_vertices;
        Normals = saved_normals;

        if (drawn)
            return 1;

        // Fall through to the CPU path rather than dropping the pedestrian. DrawNativeTransform
        // returns false for a mesh it cannot express, and a missing pedestrian is worse than an
        // unlit one.
    }

    // No lighter: the original's own unlit draw, exactly as it always did at LOW.
    if (unlit)
        return ModelDraw(flags, anim, frame);

    // --- Original CPU path -----------------------------------------------------------------------
    // Unchanged in behaviour from the assembly this replaces: skin and project via the closed
    // ModelGeometry, light into a scratch buffer with the frame's normals swapped in, restore, and
    // hand the shaded colours to FirstPass.
    if (ModelGeometry(flags, animation, frame) > 0xFF)
        return 0;

    u32* shaded = ARTS_ALLOCA(u32, AdjunctCount);

    Normals = frame_normals;

    lighter(codes, shaded, base_colors, this);

    Normals = saved_normals;

    FirstPass(shaded, TexCoords, 0);

    return 1;
}
