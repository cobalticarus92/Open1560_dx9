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

define_dummy_symbol(mmcity_inst);

#include "inst.h"

#include "agi/pipeline.h"
#include "agi/viewport.h"
#include "agiworld/meshset.h"
#include "agiworld/quality.h"
#include "arts7/sim.h"
#include "heap.h"
#include "mmcity/cullcity.h"
#include "mmcity/renderweb.h"

#include <cstring>

f32 mmInstance::LodTable[3 /*Inst Type*/][4 /*Terrain Quality*/][3 /*Lod Dist*/] {
    {
        // FACADES
        {200.0f, 150.0f, 100.0f},
        {350.0f, 250.0f, 150.0f},
        {700.0f, 500.0f, 300.0f},
        {700.0f, 500.0f, 300.0f},
    },
    {
        // BANGERS
        {100.0f, 50.0f, 20.0f},
        {150.0f, 85.0f, 35.0f},
        {250.0f, 160.0f, 60.0f},
        {250.0f, 200.0f, 150.0f},
    },
    {
        // UPPERS
        {9999.0f, 0.0f, 0.0f},
        {9999.0f, 100.0f, 0.0f},
        {9999.0f, 500.0f, 300.0f},
        {9999.0f, 600.0f, 400.0f},
    },
};

i32 mmInstance::LodTableIndex = 0;

mmInstance::MeshSetTableEntry mmInstance::MeshSetTable[MaxMeshSetSets] {};
char* mmInstance::MeshSetNames[MaxMeshSetSets] {};
i32 mmInstance::MeshSetSetCount = 0;

b32 mmInstance::ShowLights = false;
void (*mmInstance::StaticLighter)(u8*, u32*, u32*, agiMeshSet*) = nullptr;

mmHeap<i32> mmInstanceHeap {};

mmPhysEntity* mmInstance::GetEntity()
{
    return nullptr;
}

mmPhysEntity* mmInstance::AttachEntity()
{
    return nullptr;
}

Vector3 mmInstance::GetVelocity()
{
    return Vector3(0.0f, 0.0f, 0.0f);
}

void mmInstance::Impact(mmInstance* /*arg1*/, Vector3* /*arg2*/)
{}

void mmInstance::Detach()
{}

void mmInstance::Draw(i32 lod)
{
    if (Sim()->IsDebugDrawEnabled())
        return;

    if (agiMeshSet* mesh = GetResidentMeshSet(lod, 0))
    {
        Matrix34 matrix;
        Viewport()->SetWorld(ToMatrix(matrix));

        mesh->Draw(MESH_DRAW_CLIP);
    }
}

void mmInstance::DrawShadow()
{}

void mmInstance::DrawGlow()
{}

void mmInstance::Relight()
{}

usize mmInstance::SizeOf()
{
    return sizeof(*this);
}

void mmInstance::Reset()
{}

b32 mmInstance::Init(
    aconst char* /*name*/, Vector3& /*pos1*/, Vector3& /*pos2*/, i32 /*init_flags*/, aconst char* /*part*/)
{
    return false;
}

f32 mmInstance::GetScale()
{
    return 1.0f;
}

i32 mmInstance::ComputeLod(f32 dist, f32 scale)
{
    f32 scaled = dist * scale;

    f32* lods = LodTable[LodTableIndex][agiRQ.TerrainQuality];

    if (scaled >= lods[0])
        return 0;

    if (scaled >= lods[1])
        return 1;

    if (scaled >= lods[2])
        return 2;

    return 3;
}

void mmInstance::AddMeshes(aconst char* name, i32 mesh_flags, aconst char* part, Vector3* offset)
{
    GetMeshSetSet(name, mesh_flags, part, offset);
}

// ?MatrixFromPoints@@YAXAAVMatrix34@@AAVVector3@@1M@Z
ARTS_IMPORT /*static*/ void MatrixFromPoints(Matrix34& arg1, Vector3& arg2, Vector3& arg3, f32 arg4);

mmMatrixInstance::mmMatrixInstance()
    : Matrix(IDENTITY)
{
    // mmMatrixInstances don't have an underlying entity, and colliding with one (i.e the el_train) will just crash.
    // Flags |= INST_FLAG_COLLIDER;
}

void mmMatrixInstance::Hit(mmInstance* /*arg1*/)
{}

#ifdef ARTS_DEV_BUILD
void mmMatrixInstance::AddWidgets(Bank* /*arg1*/)
{}
#endif

void mmMatrixInstance::FromMatrix(const Matrix34& matrix)
{
    Matrix = matrix;
}

Vector3& mmMatrixInstance::GetPos()
{
    return Matrix.m3;
}

u32 mmMatrixInstance::SizeOf()
{
    return sizeof(*this);
}

Matrix34& mmMatrixInstance::ToMatrix([[maybe_unused]] Matrix34& matrix)
{
    return Matrix;
}

void mmStaticInstance::Relight()
{}

agiMeshSet* mmInstance::GetResidentMeshSet(i32 lod, i32 index, i32 variant)
{
    agiMeshSet* mesh = nullptr;

    if (MeshSetTableEntry* entry = GetMeshBase(index))
    {
        if (mesh = entry->Meshes[lod]; mesh && !mesh->IsFullyResident(variant))
        {
            if (lod)
            {
                if (agiMeshSet* low = entry->Meshes[lod - 1]; low && low->IsFullyResident(variant))
                    return low;
            }
        }
    }

    return mesh;
}

void mmInstance::InitMeshes(aconst char* name, i32 mesh_flags, aconst char* part, Vector3* offset)
{
    if (Flags & (INST_FLAG_COLLIDER | INST_FLAG_MOVER | INST_FLAG_40))
        mesh_flags |= MESH_SET_UV | MESH_SET_NORMAL | MESH_SET_CPV;
    else
        mesh_flags |= MESH_SET_UV | MESH_SET_NO_BOUND;

    MeshIndex = static_cast<u16>(GetMeshSetSet(name, mesh_flags, part, offset));
}

void* mmInstance::operator new(std::size_t size)
{
    return mmInstanceHeap.Allocate(size);
}

void mmInstance::operator delete(void* ptr)
{
    mmInstanceHeap.Free(ptr);
}

void mmBuildingInstance::Draw(i32 lod)
{
    enum
    {
        MESH_FACADE = 0,
        MESH_GRND = 1,
    };

    if (Sim()->IsDebugDrawEnabled())
        return;

    Matrix34 world;
    Viewport()->SetWorld(ToMatrix(world));

    if (asRenderWeb::PassMask & RENDER_PASS_TERRAIN)
    {
        if (agiMeshSet* mesh = GetMeshSet(INST_LOD_HIGH, MESH_GRND))
            mesh->DrawLitEnv(DynamicLighter, CullCity()->ShadowMap, CullCity()->EnvMatrix, MESH_DRAW_CLIP);
    }

    if (asRenderWeb::PassMask & RENDER_PASS_BUILDINGS)
    {
        if (agiMeshSet* mesh = GetResidentMeshSet(std::max(lod, INST_LOD_LOW), MESH_FACADE))
            mesh->DrawLit(StaticLighter, MESH_DRAW_CLIP, nullptr);
    }
}

// agiworld/meshrend.cpp - the OPEN1560_NATIVE_MASK gate, so facades bisect with the same switch as
// every other lit mesh. 0x2 is NATIVE_DRAWLIT: a facade quad is a DrawLit with its own corners.
bool agiNativePathEnabled(u32 which);

// ?DrawLit@mmFacadeQuad@@QAEXP6AXPAEPAI1PAVagiMeshSet@@@Z2@Z
//
// Building side facades: the LEFT/RIGHT/TOP/BACK panels mmFacadeInstance::Draw lays over a block.
// This was the one lit-mesh route the world-space work never reached, because it is not
// agiMeshSet::DrawLit - it is its own closed routine (game.asm ~183949) that ends in
// Geometry() + FirstPass(), i.e. CPU-pretransformed screen triangles. Those carry no world
// information, so RTX Remix never saw these faces at all: the reported "some sides of Chicago
// buildings" missing from a capture.
//
// What the original does, instruction for instruction:
//
//   * copies the mesh's first four vertices, and clamps each vertex's Y and Z up to this quad's
//     floors at +0x10/+0x14, when they are nonzero - a facade stretched over a lower block is cut
//     off at the ground/back plane instead of poking through it;
//   * builds four UVs from the packed 8.8 fixed-point pairs at +0x00 (i16 u, i16 v, times 1/256),
//     which is how a facade tiles its texture across a panel of arbitrary size;
//   * Geometry(1, those vertices, mesh->Planes); lighter(nullptr, shaded, mesh->Colors, mesh);
//     FirstPass(shaded, those UVs, 0) - or FirstPass(Colors, UVs, 0xFFFFFFFF) with no lighter.
//
// On a device that can take world-space geometry, the same corners and UVs go to
// agiMeshSet::DrawLit instead, which lights and submits them on the hardware path. It draws from a
// stack copy of the mesh header with only Vertices and TexCoords repointed, rather than by
// swapping those pointers on the mesh itself: a facade mesh is shared by every instance built from
// the same art, and the draw path is reached on more than one thread (see the note on the
// buffers in agiMeshSet::DrawNativeTransform), so nothing here writes to the shared object. The
// copy is raw storage and is never destructed, because agiMeshSet's destructor frees the mesh's
// data and this does not own it. Locking stays on the real mesh, which the copy shares a cache
// handle with.
//
// Everywhere else - OpenGL, software, OPEN1560_NATIVE_MASK without 0x2 - the original routine runs
// unchanged below.
void mmFacadeQuad::DrawLit(void (*lighter)(u8*, u32*, u32*, agiMeshSet*), agiMeshSet* mesh)
{
    if (!mesh->LockIfResident())
    {
        mesh->PageIn();
        return;
    }

    const i16* packed_uvs = reinterpret_cast<const i16*>(&gap0[0x00]);
    const f32 floor_y = *reinterpret_cast<const f32*>(&gap0[0x10]);
    const f32 floor_z = *reinterpret_cast<const f32*>(&gap0[0x14]);

    Vector3 verts[4];
    Vector2 uvs[4];

    for (u32 i = 0; i < 4; ++i)
    {
        verts[i] = mesh->Vertices[i];

        uvs[i].x = packed_uvs[(i * 2) + 0] * (1.0f / 256.0f);
        uvs[i].y = packed_uvs[(i * 2) + 1] * (1.0f / 256.0f);

        if ((floor_y != 0.0f) && (verts[i].y < floor_y))
            verts[i].y = floor_y;

        if ((floor_z != 0.0f) && (verts[i].z < floor_z))
            verts[i].z = floor_z;
    }

    // The original reads exactly four vertices and four UVs, so a facade mesh is four corners by
    // construction. Checked rather than assumed, because the hardware path indexes these arrays by
    // the mesh's own counts - a larger mesh keeps the original routine and its original behaviour.
    const bool world_path = Pipe()->SupportsNativeTransform() && agiNativePathEnabled(0x2) &&
        (mesh->VertexCount <= 4) && (mesh->AdjunctCount <= 4);

    if (world_path)
    {
        alignas(agiMeshSet) unsigned char storage[sizeof(agiMeshSet)];
        std::memcpy(storage, static_cast<const void*>(mesh), sizeof(agiMeshSet));

        agiMeshSet* quad = reinterpret_cast<agiMeshSet*>(storage);
        quad->Vertices = verts;
        quad->TexCoords = uvs;

        // Flags 1, as the original passes Geometry(). DrawLit falls back to its own CPU branch on
        // the copy for the cases it cannot light in hardware (no normals, agiConeLighter).
        quad->DrawLit(lighter, 1, nullptr);
    }
    else if (mesh->Geometry(1, verts, mesh->Planes) <= 0xFF)
    {
        if (lighter)
        {
            u32 shaded[4];
            lighter(nullptr, shaded, mesh->Colors, mesh);

            mesh->FirstPass(shaded, uvs, 0);
        }
        else
        {
            mesh->FirstPass(mesh->Colors, uvs, 0xFFFFFFFF);
        }
    }

    mesh->Unlock();
}
