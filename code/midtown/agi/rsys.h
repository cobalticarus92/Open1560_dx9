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

#include "refresh.h"
#include "vertex.h"

class agiMtlDef;
class agiTexDef;
class Matrix34;
class agiViewParameters;
// Second-pass material effects for the hardware-transform path, replacing the two closed
// CPU-pretransform routines the native path cannot call: agiMeshSet::SphereMap (vehicle chrome) and
// agiMeshSet::EnvMap (the ground's projected shadow/environment map). Both of those consume the
// codes/out/firstFacet scratch the CPU Geometry() pass leaves behind, which DrawNativeTransform
// never populates, so calling them from the native path crashes.
//
// Both replacements are ordinary world-space draws - model-space positions plus the same
// SetTransform the base pass used - so unlike the routines they replace they are visible to RTX
// Remix, and they reuse the base pass's vertex positions so Remix sees a second material on a mesh
// it already knows rather than a new one.
struct agiNativeMaterialFx
{
    // Vehicle sphere map, from agiMeshSet::DrawLitSph.
    agiTexDef* ReflectionTexture {};
    f32 ReflectionAmount {1.0f};

    // Embellishments, off by default - the original SphereMap pass has neither. See
    // BuildVehicleReflectionVertices.
    f32 FresnelBias {1.0f};
    f32 FresnelScale {0.0f};
    f32 SpecularBoost {0.0f};

    // Ground/terrain projected environment map, from agiMeshSet::DrawLitEnv. EnvTransform is the
    // world-to-environment matrix (mmCullCity::EnvMatrix); the pass composes it with the draw's own
    // world matrix, which is exactly what the original does.
    agiTexDef* EnvTexture {};
    const Matrix34* EnvTransform {};
};

// One texture batch of a cached world mesh: the texture slot it draws with, and its run of indices.
struct agiNativeMeshBatch
{
    u32 Texture;
    u32 FirstIndex;
    u32 IndexCount;
};

// A world mesh as the renderer keeps it - see agiRasterizer::FindNativeMesh. The arrays are the
// renderer's own copies, laid out exactly as agiMeshSet::DrawNativeTransform builds them: one vertex
// array for the whole mesh and every batch's indices back to back.
struct agiNativeCachedMesh
{
    agiWorldVtx* Vertices;
    u32 VertexCount;
    u16* Indices;
    u32 IndexCount;
    agiNativeMeshBatch* Batches;
    u32 BatchCount;

    // The renderer's own resources for it (agidx9: a vertex and an index buffer), or null.
    void* RendererData;
};

// The cached mesh the MeshWorld call in flight was built from, or null for an uncached draw. Set by
// agiMeshSet::DrawNativeTransform around each MeshWorld call, so the renderer can draw from its own
// buffers instead of the arrays it is handed. A global for the reason agiNativeReflectivity is:
// MeshWorld's signature is shared with call sites that know nothing of it. The renderer only trusts
// it when the arrays it is handed are this mesh's own, which also makes it safe to share between
// threads. Defined in agiworld/meshrend.cpp.
extern const agiNativeCachedMesh* agiNativeDrawMesh;

// True while agiMeshSet::DrawLitEnv submits through the hardware path: the draw in flight is road
// or ground. A global rather than a flag in agiNativeMaterialFx because DrawLitEnv only passes that
// struct when it has an environment map to apply, and the ground is ground either way - the RTX
// Remix wet-road layer (agidx9/dx9remixwet.cpp) keys on it. Defined in agiworld/meshrend.cpp.
extern bool agiNativeGroundDraw;

// -nocull: backface, LOD and distance culling all off, for RTX Remix captures. Defined in
// agiworld/meshrend.cpp; declared here because agidx9 and mmcity both consult it and a second
// cmd_param of the same name would register twice. Portal/cell visibility is not covered - that
// traversal is closed assembly. See the note at the definition.
bool agiNoCullEnabled();

// A skinned model submitted with the bones handed to the hardware as a matrix palette, and every
// vertex carrying the index of the bone it is bound to.
//
// agiMeshModel's binding is rigid - agiMeshModel::SkinGroupVerts partitions the vertex array into
// one contiguous run per bone, with no per-vertex weights - so a pedestrian is not a deforming mesh
// at all. It is a handful of rigid pieces on a skeleton, and skinning it on the CPU threw that
// structure away: the submitted positions changed every frame, which gave every pedestrian a new
// RTX Remix geometry hash every frame and made them unreplaceable.
//
// D3D9 fixed function expresses this binding natively: D3DRS_INDEXEDVERTEXBLENDENABLE with
// D3DRS_VERTEXBLEND = D3DVBF_0WEIGHTS means "one matrix per vertex, chosen by the vertex's own
// index, no weights" - the rigid case precisely. The bone matrices go out through
// SetTransform(D3DTS_WORLDMATRIX(i)) and the transform happens in the vertex pipeline, so no vertex
// position is ever touched by the CPU and the whole model costs one submission.
//
// A palette of ONE is the degenerate case and is not vertex blending at all - it is a single world
// matrix, which is what a renderer without a palette falls back to, submitting the model one bone
// at a time. Everything below reads the same in both cases; only the draw differs.
//
// NOTHING in the submitted vertex may depend on the animation frame, which is the whole point:
// positions are the mesh's stored bind-pose values, colours and UVs are immutable mesh data, and
// normals are rebuilt from the mesh's own geometry rather than taken from the animation's per-frame
// set (see BuildSkinBindNormals in agiworld/meshrend.cpp). A pedestrian therefore submits
// byte-identical vertices in every pose of every animation, and hashes to one mesh.
struct agiNativeSkinPalette
{
    // Count entries, each already composed as bone * world - they replace D3DTS_WORLD entirely.
    const Matrix34* Bones {};

    // Count entries, the transpose of each bone's 3x3, i.e. its inverse rotation.
    //
    // Used for ONE thing, and deliberately not per vertex: resolving which side of its own plane a
    // facet's geometric normal is meant to point at, by comparing it against the animation's stored
    // normals - which are posed for the frame, so they have to be brought back into the mesh's own
    // space before they can be compared with anything. That comparison produces a single sign bit
    // for the whole submission (see BuildSkinBindNormals), so no per-frame value reaches the
    // vertices through it.
    const Matrix34* NormalUnpose {};

    u32 Count {};

    // One entry per agiMeshSet::Vertices index, holding a GLOBAL bone index. The palette slot is
    // VertexBones[v] - FirstBone, which is what lets a skeleton larger than the device's palette be
    // submitted as several chunks without rebuilding this table per chunk.
    const u8* VertexBones {};
    u32 FirstBone {};

    // Facet range for this chunk. A facet is submitted only when every corner lands inside it,
    // which is how a chunk takes the facets belonging to its own bones and no others. Equal to the
    // whole vertex array when the palette holds the entire skeleton - the usual case, and the one
    // where no facet can be dropped for straddling a boundary because there is no boundary.
    u32 FirstVertex {};
    u32 EndVertex {};

    // Filled in by agiMeshSet::DrawNativeTransform, not by the caller: one palette slot per
    // SUBMITTED vertex, in the order that path emits them. VertexBones above is indexed by mesh
    // vertex, and the submitted vertices are adjuncts (or, under -flatnormals, unshared facet
    // corners), so only that function knows the mapping. Everything outside the chunk is clamped
    // into range - those vertices are unreferenced by the indices, but they are still inside the
    // range handed to DrawIndexedPrimitiveUP and an out-of-palette index is undefined behaviour.
    const u8* Slots {};
};

class agiRasterizer : public agiRefreshable
{
public:
    // ??0agiRasterizer@@QAE@PAVagiPipeline@@@Z
    ARTS_EXPORT agiRasterizer(agiPipeline* pipe);

    // ??1agiRasterizer@@UAE@XZ
    ~agiRasterizer() override = default;

    // ?BeginGroup@agiRasterizer@@UAEXXZ
    virtual void BeginGroup();

    // ?EndGroup@agiRasterizer@@UAEXXZ
    virtual void EndGroup();

    virtual void Verts(agiVtxType type, agiVtx* vertices, i32 vertex_count) = 0;

    virtual void Points(agiVtxType type, agiVtx* vertices, i32 vertex_count) = 0;

    virtual void SetVertCount(i32 vertex_count) = 0;

    virtual void Triangle(i32 v0, i32 v1, i32 v2) = 0;

    // ?Quad@agiRasterizer@@UAEXHHHH@Z
    virtual void Quad(i32 v0, i32 v1, i32 v2, i32 v3);

    // ?Poly@agiRasterizer@@UAEXPAHH@Z
    ARTS_EXPORT virtual void Poly(i32* indices, i32 count);

    virtual void Line(i32 v0, i32 v1) = 0;

    virtual void Card(i32 v0, i32 v1) = 0;

    virtual void Mesh(agiVtxType type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count) = 0;

    // ?Mesh2@agiRasterizer@@UAEXPAUagiScreenVtx2@@HPAGH@Z
    ARTS_EXPORT virtual void Mesh2(agiScreenVtx2* vertices, i32 vertex_count, u16* indices, i32 index_count);

    // ?LineList@agiRasterizer@@UAEXW4agiVtxType@@PATagiVtx@@H@Z
    ARTS_EXPORT virtual void LineList(agiVtxType type, agiVtx* vertices, i32 vertex_count);

    // Not part of the original engine/binary - appended after every original virtual so the
    // vtable slots the assembly relies on (by original offset) are left undisturbed.
    // Lets a renderer with native hardware transform/lighting (agidx9) draw a facet directly
    // from untransformed model-space data, bypassing agiScreenVtx/RAST->Mesh entirely. Default
    // implementation reports "unsupported" so every other renderer is unaffected.
    // static_lighting selects which CPU lighting model this draw would otherwise have used, so
    // the renderer can replicate the right one in hardware: false = the real-time dynamic light
    // list (agiLighter::LIGHTS[]/SceneAmbient, correct for cars/wheels/moving objects), true =
    // the fixed sun/fill1/fill2 + ambient rig (agiMeshLighterSun/Fill1/Fill2/Ambient) that
    // agiMeshLighterTriple/Quarter compute for StaticLighter/DynamicLighter-lit city geometry
    // (buildings, terrain) - these are two genuinely different, unrelated lighting sources in
    // the original engine, not degrees of the same one.
    // hardware_lighting: false means the vertices already carry their final colors and the
    // renderer must light nothing (D3DRS_LIGHTING off, vertex diffuse used as-is). Meshes loaded
    // without MESH_SET_NORMAL have no per-vertex normals at all - the CPU path draws them straight
    // from their baked agiMeshSet::Colors, and agiMeshLighter* would fault on mesh->Normals[i] if
    // asked to light them - so they can still take this path, just unlit. The `normal` field of
    // the submitted vertices is then meaningless and must not be read.
    // skin: when set, the draw is a hardware-skinned submission - see agiNativeSkinPalette. `world`
    // is then unused, because the palette's matrices already carry it.
    virtual bool MeshWorld(agiWorldVtx* vertices, i32 vertex_count, u16* indices, i32 index_count,
        const Matrix34& world, const Matrix34& view, const agiViewParameters& proj_params, bool static_lighting,
        const agiNativeMaterialFx* fx = nullptr, bool hardware_lighting = true,
        const agiNativeSkinPalette* skin = nullptr)
    {
        (void) vertices;
        (void) vertex_count;
        (void) indices;
        (void) index_count;
        (void) world;
        (void) view;
        (void) proj_params;
        (void) static_lighting;
        (void) fx;
        (void) hardware_lighting;
        (void) skin;

        return false;
    }

    // How many bones one skinned draw may carry.
    //
    // A caller with more bones than this splits the skeleton into chunks of this size, so this only
    // decides how many draws a skinned model costs, never whether it can be drawn. One - the
    // default - means no matrix palette: each bone is submitted on its own against a plain world
    // matrix, which is something every renderer that implements MeshWorld() can already do. Asked
    // once per model draw rather than cached, because it depends on device state (see
    // agiDX9Rasterizer).
    virtual u32 MaxNativeSkinBones() const
    {
        return 1;
    }

    // Not part of the original engine/binary - appended after every other virtual, as MeshWorld was.
    //
    // A world mesh built once and kept by the renderer, so a mesh drawn every frame is neither rebuilt
    // on the CPU nor resubmitted in full every frame. See agiNativeCachedMesh. `key` is a hash of
    // everything the build reads (agiMeshSet::DrawNativeTransform computes it), so geometry that
    // changes - a dented car, a tinted draw - simply has a new key. Find returns null when the renderer
    // does not cache, or has not got it. Store may return null too (not seen often enough yet, or no
    // room), and the caller then draws from its own arrays as before. What either returns stays valid
    // until the next frame begins. CachesNativeMeshes says whether the key is worth computing at all.
    virtual bool CachesNativeMeshes()
    {
        return false;
    }

    virtual const agiNativeCachedMesh* FindNativeMesh(u64 key)
    {
        (void) key;
        return nullptr;
    }

    virtual const agiNativeCachedMesh* StoreNativeMesh(u64 key, const agiWorldVtx* vertices, u32 vertex_count,
        const u16* indices, u32 index_count, const agiNativeMeshBatch* batches, u32 batch_count)
    {
        (void) key;
        (void) vertices;
        (void) vertex_count;
        (void) indices;
        (void) index_count;
        (void) batches;
        (void) batch_count;
        return nullptr;
    }
};

check_size(agiRasterizer, 0x18);

enum class agiBlendSet : u8
{
    SrcAlpha_InvSrcAlpha = 0,
    SrcAlpha_One = 1,
    Zero_SrcAlpha = 3,
    Zero_SrcColor = 4,
    One_One = 5, // Additive Blending
};

enum class agiCullMode : u8
{
    None = 1, // Do not cull back faces
    CW = 2,   // Cull back faces with clockwise vertices
    CCW = 3   // Cull back faces with counterclockwise vertices
};

enum class agiCmpFunc : u8 // gfxZFunc
{
    Never = 1,
    Less = 2,
    Equal = 3,
    LessEqual = 4,
    Greater = 5,
    Notequal = 6,
    GreaterEqual = 7,
    Always = 8,
};

enum class agiTexFilter : u8
{
    Point = 0,     // Point
    Bilinear = 1,  // Linear
    Trilinear = 2, // Nicest
};

enum class agiTexEnv : u8
{
    // Color = Texture
    // COLOROP = SELECTARG1, COLORARG1 = TEXTURE, ALPHAOP = SELECTARG1, ALPHAARG1 = TEXTURE
    Replace = 0,

    // Color = Texture * Diffuse
    // COLOROP = MODULATE,   COLORARG1 = TEXTURE, ALPHAOP = MODULATE,   ALPHAARG1 = TEXTURE, COLORARG2 = DIFFUSE
    Modulate = 1,

    // Color = Diffuse
    // COLOROP = DISABLE
    Disable = 2,
};

enum class agiFogMode : u8
{
    None,
    Pixel,
    Vertex,
};

enum class agiFillMode : u8
{
    Point = 0x1,
    Wire = 0x2,
    Solid = 0x3,
};

enum agiDrawMode : u8
{
    agiDrawFillMask = 0x3,

#define X(DRAW, FILL) (DRAW << 2) | static_cast<u8>(agiFillMode::FILL)
    agiDrawWireframe = X(0, Wire),
    agiDrawDepth = X(0, Solid),
    agiDrawColored = X(1, Solid),
    agiDrawSolid = X(2, Solid),
    agiDrawTextured = X(3, Solid),
#undef X
};

struct agiRendStateStruct
{
public:
    // ?Reset@agiRendStateStruct@@QAEXXZ
    void Reset();

    agiMtlDef* Mtl {};
    agiTexDef* Texture {};
    agiTexDef* Texture2 {};

    agiBlendSet BlendSet {};

    // false: Flat (Provoking Vertex)
    // true : Interpolate
    bool SmoothShading {};

    agiDrawMode DrawMode {};

    agiTexFilter TexFilter {};

    agiTexEnv TexEnv {};

    agiCullMode CullMode {};

    agiCmpFunc ZFunc {};

    agiFogMode FogMode {};
    bool TexturePerspective {};
    bool AlphaEnable {};

    bool WrapU {};
    bool WrapV {};

    bool ZEnable {};
    bool ZWrite {};

    u32 FogColor {};
    f32 FogStart {};
    f32 FogEnd {};
    f32 FogDensity {};

    bool Dither {};
    u8 byte2D {};
    bool SoftwareRendering {};
    bool SpecularEnable {};
    u8 byte30 {};
    i8 MaxTextures {};
    bool StippledAlpha {};
    u8 AlphaRef {};
    f32 LodBias {};
    u32 Specular {};
};

check_size(agiRendStateStruct, 0x3C);

class agiRendState
{
private:
    b32 touched_ {};
    agiRendStateStruct state_ {};

public:
    bool IsTouched()
    {
        return touched_;
    }

    void ClearTouched()
    {
        touched_ = false;
    }

    template <typename T>
    inline T Set(T& value, T new_value)
    {
        T old_value = value;

        if (old_value != new_value)
        {
            value = new_value;
            touched_ = true;
        }

        return old_value;
    }

#define AGI_RSTATE_MEMBER(NAME)                        \
    inline auto Get##NAME() const                      \
    {                                                  \
        return state_.NAME;                            \
    }                                                  \
                                                       \
    inline auto Set##NAME(decltype(state_.NAME) value) \
    {                                                  \
        return Set(state_.NAME, value);                \
    }

    AGI_RSTATE_MEMBER(Mtl)
    AGI_RSTATE_MEMBER(Texture)
    AGI_RSTATE_MEMBER(Texture2)
    AGI_RSTATE_MEMBER(BlendSet)
    AGI_RSTATE_MEMBER(SmoothShading)
    AGI_RSTATE_MEMBER(DrawMode)
    AGI_RSTATE_MEMBER(TexFilter)
    AGI_RSTATE_MEMBER(TexEnv)
    AGI_RSTATE_MEMBER(CullMode)
    AGI_RSTATE_MEMBER(ZFunc)
    AGI_RSTATE_MEMBER(FogMode)
    AGI_RSTATE_MEMBER(TexturePerspective)
    AGI_RSTATE_MEMBER(AlphaEnable)
    AGI_RSTATE_MEMBER(WrapU)
    AGI_RSTATE_MEMBER(WrapV)
    AGI_RSTATE_MEMBER(ZEnable)
    AGI_RSTATE_MEMBER(ZWrite)
    AGI_RSTATE_MEMBER(FogColor)
    AGI_RSTATE_MEMBER(FogStart)
    AGI_RSTATE_MEMBER(FogEnd)
    AGI_RSTATE_MEMBER(FogDensity)
    AGI_RSTATE_MEMBER(Dither)
    AGI_RSTATE_MEMBER(byte2D)
    AGI_RSTATE_MEMBER(SoftwareRendering)
    AGI_RSTATE_MEMBER(SpecularEnable)
    AGI_RSTATE_MEMBER(byte30)
    AGI_RSTATE_MEMBER(MaxTextures)
    AGI_RSTATE_MEMBER(StippledAlpha)
    AGI_RSTATE_MEMBER(AlphaRef)
    AGI_RSTATE_MEMBER(LodBias)
    AGI_RSTATE_MEMBER(Specular)

#undef AGI_RSTATE_MEMBER
};

check_size(agiRendState, 0x40);

// ?RAST@@3PAVagiRasterizer@@A
ARTS_EXPORT extern agiRasterizer* RAST;

// Unused
struct agiRenderOpts
{};

// ?ROPTS@@3UagiRenderOpts@@A
extern agiRenderOpts ROPTS;

// ?agiCurState@@3VagiRendState@@A
ARTS_EXPORT extern agiRendState agiCurState;

// ?agiLastState@@3UagiRendStateStruct@@A
extern agiRendStateStruct agiLastState;
