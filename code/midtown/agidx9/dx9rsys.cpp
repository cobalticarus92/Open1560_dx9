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

#include "dx9rsys.h"

#include "agi/error.h"
#include "agi/light.h"
#include "agi/mtldef.h"
#include "agi/pipeline.h"
#include "agi/rsys.h"
#include "agi/texdef.h"
#include "agi/viewport.h"
#include "agirend/lighter.h"
#include "agiworld/glowlight.h"
#include "agiworld/meshlight.h"
#include "agiworld/meshset.h"
#include "agiworld/quality.h"
#include "agiworld/texsheet.h" // agiTexProp::AlphaGlow
#include "data7/utimer.h"
#include "eventq7/active.h"
#include "memory/alloca.h"
#include "pcwindis/setupdata.h"
#include "vector7/matrix34.h"

#include "dx9context.h"
#include "dx9ffshade.h"
#include "dx9shader.h"
#include "dx9texdef.h"

#include "dx9_windows.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

define_dummy_symbol(agidx9_dx9rsys);

// D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1 - exactly matches agiScreenVtx's layout
static constexpr DWORD kScreenVtxFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
static constexpr DWORD kWorldReflectFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// NOTE: at namespace scope deliberately. A function-local `static mem::cmd_param` is constructed
// lazily on first call, which happens long after mem::cmd_param::init() has already walked argv
// and assigned values to every registered parameter - so it registers too late and silently
// never receives its value, no matter what the user passes on the command line.
static mem::cmd_param PARAM_d3d9_depthbias {"d3d9depthbias", "Depth bias for hardware-transformed geometry"};

// -ghash: does this engine's geometry hash the SAME way every frame?
//
// RTX Remix identifies a mesh by hashing it at DRAW time - vertex bytes for the drawn range, index
// bytes, stride, vertex count, primitive type. Not textures, not transforms, not render state. That
// hash is the key into the replacement database, so a mesh whose bytes differ frame to frame gets a
// new identity every frame: replacements never stick, and the capture fills with thousands of
// one-frame meshes. This is the failure the Max Payne d3d8 proxy work was built to fix, and the
// hashing model here is taken from its analysis (MaxPayne_RTX_Remix_Hash_Stabilization_Analysis.md
// §2, on D: - the D3D8 and D3D9 Remix runtimes hash the same way).
//
// The number that matters is CHURN (new hashes per frame), not the hash values. A steady scene that
// keeps producing new hashes is broken for Remix even though it looks perfect on screen.
//
// The expectation for Pathway A is that churn settles to ~zero once a view stabilises, because
// MeshWorld submits MODEL-space vertices - the world matrix goes out through SetTransform and never
// touches the vertex bytes. If churn does NOT settle, something is rewriting vertex data per frame
// and that is a real Remix blocker worth finding. Either way this answers it with a number instead
// of a guess, which is the whole point.
//
// That expectation was too narrow, and this measured the failure it missed. The vertex bytes were
// never the problem; the INDEX bytes were. agiMeshSet::DrawNativeTransform built its index array by
// filtering facets through a camera-dependent backface test, so churn tracked camera MOTION rather
// than settling with the view - a still camera looked clean and a moving one rehashed the whole
// city. See the facet-order note in agiworld/meshrend.cpp for the full account. Churn should now be
// zero for anything that is not genuinely animated on the CPU (pedestrians, damaged bodywork).
static mem::cmd_param PARAM_ghash {"ghash", "Report RTX Remix geometry hash stability for world draws"};

// The visual half: Remix's Debug View -> Geometry Hash, reproduced on the fixed-function path.
// Tints every world draw flat with a colour derived from its geometry hash. Stable geometry holds
// its colour; anything rehashed per frame strobes. See agiDX9GHashColor.
static mem::cmd_param PARAM_ghashcolor {"ghashcolor", "Tint world draws by RTX Remix geometry hash (debug view)"};

// -d3d9nofx: drop the chrome and ground-env second passes.
//
// Both resubmit the base pass's positions and indices under a different vertex layout, with UVs
// recomputed from the camera every frame - BuildVehicleReflectionVertices indexes a sphere map by
// the reflection vector, BuildGroundEnvVertices by a planar projection through the env transform.
// Positions and indices are the base pass's, so under a hash rule of positions + indices +
// geometry descriptor these pick up their own stable identity; under any rule that includes
// texcoords they are a brand new mesh every frame. Either way they push a second copy of every car
// body and every stretch of road into a renderer that does reflections properly by itself.
//
// Off by default - this costs real visuals (chrome on the cars, the ground map on the road) and
// only earns its keep when capturing for Remix.
static mem::cmd_param PARAM_d3d9_nofx {
    "d3d9nofx", "Skip the reflection and ground-env second passes (they duplicate geometry for RTX Remix)"};

// -aniso: anisotropic texture filtering.
//
// MM1 is a game viewed almost entirely at grazing angles - the road surface fills the lower half of
// every frame and runs to the horizon - which is precisely the case trilinear filtering handles
// worst. Mip selection there is driven by the larger of the two screen-space derivatives, so a
// surface compressed hard along one axis and barely at all along the other gets a mip chosen for the
// compressed axis and blurs away detail the other axis still had. That is the smeared, low-contrast
// asphalt this engine has always had at distance, and it is a filtering artifact rather than a
// texture-resolution one.
//
// Default 16, clamped to what the device reports. This is free on any GPU that can run the Remix
// bridge, and there is no version of this project's target hardware where it is not.
//
// Off with -aniso 1. The level is resolved once at BeginGfx rather than per bind, because
// D3DCAPS9::MaxAnisotropy needs a device and the answer cannot change while one exists.
static mem::cmd_param PARAM_aniso {"aniso", "Anisotropic filtering level (1 = off, default 16, clamped to device max)"};

static DWORD g_MaxAnisotropy = 1;

// Escape hatch for the projection reset in RestoreStateAfterWorldDraw - see the long note there.
// Off by default: resetting it is what stops RTX Remix path tracing the moment the frame's first
// blob shadow goes out.
static mem::cmd_param PARAM_d3d9_identityproj {
    "d3d9identityproj", "Reset PROJECTION to identity after world draws (breaks RTX Remix)"};

// Escape hatch for the depth-range change described at BuildProjectionMatrix() and re-asserted in
// agiDX9Pipeline::BeginFrame(). Off by default: folding the engine's depth guard band into the
// projection matrix is what stops RTX Remix path tracing in gameplay.
// -d3d9rhview. Hands RTX Remix a right-handed view matrix and moves the Z flip into the projection.
// Mathematically identical for rasterisation - see the derivation at BuildProjectionMatrix - so this
// is an A/B switch for a Remix-side hypothesis rather than a rendering change. Off by default
// because it does change two fixed-function behaviours that read D3DTS_VIEW directly; see MeshWorld.
static mem::cmd_param PARAM_d3d9_rhview {
    "d3d9rhview", "Give RTX Remix a right-handed view matrix, folding the Z flip into the projection"};

mem::cmd_param PARAM_d3d9_legacydepth {
    "d3d9legacydepth", "Fold agiMeshSet::DepthScale/DepthOffset into PROJECTION (breaks RTX Remix)"};

// -d3d9worldfog: whether hardware-transformed (world) draws carry fixed-function fog.
//
// RTX Remix does not ignore D3D9 fog. It reads FOGENABLE/FOGTABLEMODE/FOGSTART/FOGEND/FOGCOLOR off
// the draws it path traces and rebuilds them as its own volumetric fog. Once 2c74846 put
// D3DFOG_LINEAR back on world draws (start 1, end mmCullCity's FogEnd, colour SkyColor), Remix
// received a real fog ramp for the first time, and in its units that ramp closes within a few metres
// of the camera. The whole scene comes out in fog colour, which is the reported "cars and road look
// white". 72a6dba, the last build reported correct under Remix, sent FOGTABLEMODE = FOGVERTEXMODE =
// NONE on those draws, which Remix reads as no fog at all.
//
// Plain D3D9 needs that fog, because rasterised fog is the only distance cue the city has. So the
// default follows the device: off when the Remix bridge is detected, on otherwise. -d3d9worldfog 1
// forces it on, for tuning Remix's own fog. -d3d9worldfog 0 forces it off, for a Remix install that
// agiDX9RemixBridgeActive() does not recognise.
static mem::cmd_param PARAM_d3d9_worldfog {
    "d3d9worldfog", "Fixed-function fog on world draws (default: off under RTX Remix, on otherwise)"};

static bool agiDX9WorldFogWanted()
{
    if (bool forced = false; PARAM_d3d9_worldfog.get(forced))
        return forced;

    return !agiDX9RemixBridgeActive();
}

// -d3d9nostatecache: send every render state, stage state, transform, FVF and stage-0 binding to
// the device, even when the value has not changed. This turns off the filter in front of
// agiDX9Rasterizer (agiDX9WorldStateCache).
//
// It is a diagnostic switch. The filter only drops a call the device already agrees with, so
// turning it off should change nothing but speed. If the picture changes with it on, some writer
// is reaching the device without going through the filter or invalidating it, and that is a bug to
// find. Read once, at the first write.
static mem::cmd_param PARAM_d3d9_nostatecache {
    "d3d9nostatecache", "Send every device state write, even unchanged ones (diagnostic)"};

static bool agiDX9StateCacheEnabled()
{
    static const bool enabled = !PARAM_d3d9_nostatecache.get_or(false);

    return enabled;
}

static u32 ImmPrimType = D3DPT_TRIANGLELIST;

static agiVtx* ImmVtxBase = nullptr;
static u32 ImmVtxCount = 0;

alignas(16) static u16 ImmIdxBuffer[4096];
static u32 ImmIdxCount = 0;

struct DX9ReflectVtx
{
    Vector3 pos;
    u32 color;
    f32 tu;
    f32 tv;
};

// The hardware-skinned vertex - agiWorldVtx with the bone index spliced in.
//
// The layout is not a choice. D3D9's FVF ordering puts the blend weights immediately after the
// position and before the normal, and D3DFVF_LASTBETA_UBYTE4 reinterprets the last (here only)
// beta DWORD as four bytes of matrix INDEX rather than a float weight. Under D3DVBF_0WEIGHTS only
// the first of those bytes is read - one matrix per vertex, no weights - which is exactly how a
// pedestrian is bound. The remaining three bytes are unused and written zero so the vertex bytes
// stay a pure function of the mesh, for the geometry hash's sake.
struct DX9SkinVtx
{
    Vector3 pos;
    u32 indices;
    Vector3 normal;
    u32 color;
    f32 tu;
    f32 tv;
};

static constexpr DWORD kWorldSkinFVF =
    D3DFVF_XYZB1 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// -noskin. Force a palette of one: a skinned model is submitted one bone at a time against a plain
// world matrix, with no vertex blending anywhere.
//
// Kept because the palette changes what an RTX Remix capture sees. Remix reconstructs a draw's world
// placement from D3DTS_WORLD, and a vertex-blended draw's placement lives in the
// D3DTS_WORLDMATRIX(i) palette instead - one bone per draw hands it one plain world matrix, which is
// unambiguous. Nothing else differs: both submit the same bind-pose positions and the same
// frame-independent normals, so the geometry hashes are equally stable either way.
static mem::cmd_param PARAM_noskin {"noskin", "Disable hardware matrix-palette skinning (submit one draw per bone)"};

static ARTS_FORCEINLINE f32 Clamp01(f32 value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

static ARTS_FORCEINLINE Vector3 NormalizeSafe(const Vector3& value, const Vector3& fallback)
{
    f32 mag2 = value.Mag2();
    return (mag2 > 1.0e-8f) ? (value / std::sqrt(mag2)) : fallback;
}

// Sphere-map UVs for a vehicle body, reproducing agiMeshSet::SphereMap.
//
// The mapping is not a matter of taste - the game ships authored sphere maps, and reading them at
// the wrong coordinates gives a reflection that is merely *a* reflection rather than the one the car
// was built to wear. So this follows the original exactly, decoded from game.asm ~333870:
//
//     I  = normalize(pos * ModelView)                 // eye-to-vertex, VIEW space, translation in
//     N  = normal * ModelView (3x3)                   // VIEW space, deliberately not renormalised
//     R  = I - 2*(N.I)*N                              // reflect
//     Rw = R * Camera (3x3)                           // ...then rotate into WORLD space
//     s  = 0.5 / sqrt(Rw.x^2 + Rw.y^2 + (Rw.z + 1)^2) // constants at flt_62084C/620860/62086C
//     tu = s*Rw.x + 0.5,  tv = s*Rw.y + 0.5           // s <= 0 falls back to (0.5, 0.5)
//
// Three details are easy to get wrong and all three were wrong in the first version of this
// function, written for the programmable path against the textbook formula rather than against the
// game:
//
//   * The sphere map is indexed by the reflection vector in WORLD space, not camera space. Camera
//     space is what the textbook says and it is self-consistent, but it spins the reflection with
//     the camera instead of with the car, so a parked car's chrome slides around as you orbit it.
//   * V is NOT flipped. The textbook mapping pairs tv = 0.5 - Ry/m with a bottom-up texture origin;
//     this engine's sphere maps are stored the other way up and the original adds rather than
//     subtracts. Flipping it mirrors the reflected world top to bottom.
//   * The eye vector needs ModelView's TRANSLATION. Building it from a 3x3-only product drops the
//     camera's position, which makes every vertex behave as though the camera were at the world
//     origin - so the reflection barely changes as the car drives around.
//
// The fresnel and sun-glint terms are embellishments the original has no trace of, and they default
// to off (FresnelBias 1, FresnelScale 0, SpecularBoost 0 in agiNativeMaterialFx) so this path is a
// reproduction unless someone asks for more.
static void BuildVehicleReflectionVertices(DX9ReflectVtx* output, agiWorldVtx* input, i32 count, const Matrix34& world,
    const Matrix34& view, const agiNativeMaterialFx& fx)
{
    const agiViewParameters& params = ViewParams();

    // ModelView as the original reads it (agiViewport::Active + 0xEC), rebuilt from this draw's own
    // world matrix rather than read from the viewport: DrawNativeTransform submits several meshes
    // per instance (a car's four wheels back to back) and the shared ModelView can lag a SetWorld().
    Matrix34 model_view;
    model_view.Dot(world, view);

    Vector3 sun_dir_world = NormalizeSafe(-agiMeshLighterSun, {0.0f, 1.0f, 0.0f});

    constexpr f32 kHighlightExponent = 28.0f;

    for (i32 i = 0; i < count; ++i)
    {
        output[i].pos = input[i].pos;

        Vector3 eye_dir;
        eye_dir.Dot(input[i].pos, model_view);
        eye_dir = NormalizeSafe(eye_dir, {0.0f, 0.0f, 1.0f});

        Vector3 normal_view;
        normal_view.Dot3x3(input[i].normal, model_view);

        Vector3 reflect_view = eye_dir - (normal_view * (2.0f * (normal_view ^ eye_dir)));

        Vector3 reflect_world;
        reflect_world.Dot3x3(reflect_view, params.Camera);

        f32 z1 = reflect_world.z + 1.0f;
        f32 m = std::sqrt((reflect_world.x * reflect_world.x) + (reflect_world.y * reflect_world.y) + (z1 * z1));

        if (m > 1.0e-5f)
        {
            f32 s = 0.5f / m;

            output[i].tu = (s * reflect_world.x) + 0.5f;
            output[i].tv = (s * reflect_world.y) + 0.5f;
        }
        else
        {
            output[i].tu = 0.5f;
            output[i].tv = 0.5f;
        }

        // Grazing angles reflect more. With the defaults this collapses to a constant
        // ReflectionAmount, which is the original's flat SphMapColor behaviour.
        Vector3 view_dir = -eye_dir;
        f32 ndotv = Clamp01(NormalizeSafe(normal_view, {0.0f, 0.0f, 1.0f}) ^ view_dir);
        f32 fresnel = fx.FresnelBias + (fx.FresnelScale * std::pow(1.0f - ndotv, 5.0f));

        f32 sun_glint = 0.0f;

        if (fx.SpecularBoost > 0.0f)
        {
            Vector3 normal_world;
            normal_world.Dot3x3(normal_view, params.Camera);
            normal_world = NormalizeSafe(normal_world, {0.0f, 1.0f, 0.0f});

            Vector3 view_world;
            view_world.Dot3x3(view_dir, params.Camera);
            view_world = NormalizeSafe(view_world, {0.0f, 0.0f, 1.0f});

            Vector3 half_vec = NormalizeSafe(sun_dir_world + view_world, view_world);
            sun_glint = std::pow(Clamp01(normal_world ^ half_vec), kHighlightExponent) * 0.35f * fx.SpecularBoost;
        }

        f32 intensity = Clamp01((fx.ReflectionAmount * fresnel) + sun_glint);

        output[i].color = 0x00FFFFFF | (static_cast<u32>(intensity * 255.0f) << 24);
    }
}

// Ground/terrain projected environment map, reproducing agiMeshSet::EnvMap.
//
// Decoded from game.asm ~333253. The original opens with
//     Matrix34::Dot(local, agiViewport::Active + 0xA4, transform)
// and 0xA4 is agiViewParameters::World, so the matrix it projects through is world * EnvMatrix -
// model space straight to environment space. Per vertex it then takes
//     tu = (v * combined).x,  tv = (v * combined).y
// with no divide and no reflection vector: it is a plain affine planar projection, which is what a
// baked ground shadow map wants. The vertex colour is the caller's (0xFFFFFFFF from DrawLitEnv).
static void BuildGroundEnvVertices(
    DX9ReflectVtx* output, agiWorldVtx* input, i32 count, const Matrix34& world, const Matrix34& env_transform)
{
    Matrix34 combined;
    combined.Dot(world, env_transform);

    for (i32 i = 0; i < count; ++i)
    {
        Vector3 projected;
        projected.Dot(input[i].pos, combined);

        output[i].pos = input[i].pos;
        output[i].color = 0xFFFFFFFF;
        output[i].tu = projected.x;
        output[i].tv = projected.y;
    }
}

// Shared submission for both second passes. They differ only in how the UVs were built, which
// texture is bound and how the result is composited; everything else - unlit, no depth write, two
// sided, model-space positions against the world matrix the base pass already set - is the same.
static void DrawMaterialFxPass(IDirect3DDevice9* device, IDirect3DTexture9* texture, DX9ReflectVtx* verts,
    i32 vertex_count, u16* indices, i32 index_count, DWORD src_blend, DWORD dst_blend, bool alpha_from_diffuse)
{
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, src_blend);
    device->SetRenderState(D3DRS_DESTBLEND, dst_blend);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    device->SetTexture(0, texture);
    device->SetFVF(kWorldReflectFVF);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, alpha_from_diffuse ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, alpha_from_diffuse ? D3DTA_DIFFUSE : D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->DrawIndexedPrimitiveUP(
        D3DPT_TRIANGLELIST, 0, vertex_count, index_count / 3, indices, D3DFMT_INDEX16, verts, sizeof(DX9ReflectVtx));

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // Restore what agiLastState believes, not an unconditional TRUE. FlushState() only re-issues
    // D3DRS_ZWRITEENABLE when agiCurState's value *changes*, so forcing it on here left depth
    // writes enabled behind the cache's back for every following draw that didn't happen to toggle
    // it - notably the alpha-blended sprite passes (glows, coronas, smoke) that deliberately run
    // with ZWrite off, which then punched their quads into the depth buffer and occluded geometry
    // behind them. MeshWorld()'s own restore block does not cover ZWrite, so it has to happen here.
    device->SetRenderState(D3DRS_ZWRITEENABLE, agiLastState.ZWrite ? TRUE : FALSE);

    agiLastState.Texture = nullptr;
}

// `view` is the engine's own view matrix, which BuildVehicleReflectionVertices needs because it
// rebuilds ModelView from it exactly as the original SphereMap does. `device_view` is the Z-flipped
// one MeshWorld actually put on the device, which is the space D3D's texgen works in. They are not
// interchangeable and using one for the other mirrors the reflection through Z.
static void DrawVehicleReflectionPass(IDirect3DDevice9* device, agiWorldVtx* vertices, i32 vertex_count, u16* indices,
    i32 index_count, const Matrix34& world, const Matrix34& view, const Matrix34& device_view,
    const agiNativeMaterialFx& fx, agiDX9FFPerPixel* per_pixel)
{
    if (!fx.ReflectionTexture)
        return;

    IDirect3DTexture9* reflection_texture = static_cast<agiDX9TexDef*>(fx.ReflectionTexture)->GetHandle();

    if (!reflection_texture)
        return;

    // -ffperpixelreflect: let the hardware generate the reflection coordinates per fragment instead
    // of computing them per vertex below.
    //
    // The CPU form is exact - it reproduces SphereMap's projection term for term - but it samples
    // the reflection at VERTICES and the rasteriser then interpolates the resulting UVs. On MM1's
    // low-poly bodywork that means a whole door panel's reflection is a bilinear blend of four
    // corner reflections, which swims as the car moves. Texgen trades the exact projection for a
    // per-fragment one. See agiDX9FFPerPixel::SetupReflectionStage.
    if (per_pixel && agiDX9PerPixelReflectEnabled())
    {
        {
            device->SetRenderState(D3DRS_LIGHTING, FALSE);
            device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

            per_pixel->SetupReflectionStage(device, 0, reflection_texture, device_view);

            // Reflection strength rides in the texture factor rather than in per-vertex alpha,
            // because there are no per-vertex values on this path any more - the base pass's own
            // vertices are reused untouched, which is also what keeps the geometry Remix sees
            // identical to the base draw.
            const u32 alpha = static_cast<u32>(std::clamp(fx.ReflectionAmount, 0.0f, 1.0f) * 255.0f);

            device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(alpha, 255, 255, 255));

            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

            device->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);

            device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertex_count, index_count / 3, indices,
                D3DFMT_INDEX16, vertices, sizeof(agiWorldVtx));

            // Texgen and the stage-0 texture transform must not leak into the next draw - they would
            // silently replace its UVs with reflection vectors.
            device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            device->SetTexture(0, nullptr);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, agiLastState.ZWrite ? TRUE : FALSE);

            agiLastState.Texture = nullptr;
            return;
        }
    }

    DX9ReflectVtx* reflect_verts = ARTS_ALLOCA(DX9ReflectVtx, vertex_count);
    BuildVehicleReflectionVertices(reflect_verts, vertices, vertex_count, world, view, fx);

    // SRCALPHA/ONE: chrome adds light to the paint underneath rather than replacing it, and the
    // per-vertex alpha carries the reflection strength. Alpha comes from the diffuse only - the
    // sphere map's own alpha channel, where it has one, is not a reflectivity mask.
    DrawMaterialFxPass(device, reflection_texture, reflect_verts, vertex_count, indices, index_count, D3DBLEND_SRCALPHA,
        D3DBLEND_ONE, /*alpha_from_diffuse=*/true);
}

static void DrawGroundEnvPass(IDirect3DDevice9* device, agiWorldVtx* vertices, i32 vertex_count, u16* indices,
    i32 index_count, const Matrix34& world, const agiNativeMaterialFx& fx)
{
    if (!fx.EnvTexture || !fx.EnvTransform)
        return;

    IDirect3DTexture9* env_texture = static_cast<agiDX9TexDef*>(fx.EnvTexture)->GetHandle();

    if (!env_texture)
        return;

    DX9ReflectVtx* env_verts = ARTS_ALLOCA(DX9ReflectVtx, vertex_count);
    BuildGroundEnvVertices(env_verts, vertices, vertex_count, world, *fx.EnvTransform);

    // SRCALPHA/INVSRCALPHA, with alpha taken from the texture: the ground map is a baked shadow, so
    // it must be able to darken the road, which an additive blend cannot do. The original reached
    // the same place by handing the pass to agiTexSorter::GetEnv and letting DoTexture bind blend
    // state from the texture's own flags; this path bypasses the sorter, so it binds it here - the
    // same omission already fixed for the texture, blend and fog binds in MeshWorld.
    DrawMaterialFxPass(device, env_texture, env_verts, vertex_count, indices, index_count, D3DBLEND_SRCALPHA,
        D3DBLEND_INVSRCALPHA, /*alpha_from_diffuse=*/false);
}

agiDX9Rasterizer::agiDX9Rasterizer(agiPipeline* pipe)
    : agiRasterizer(pipe)
{}

agiDX9Rasterizer::~agiDX9Rasterizer() = default;

// --- World render-state cache -----------------------------------------------------------------
//
// MeshWorld() programs the whole fixed-function pipeline from scratch on every submission, and the
// census counts 706-1007 of those a frame. After the lights and the restore were dealt with, ~27
// device calls per draw were left, and between two consecutive city meshes almost none of them
// carries a different value: the view and projection matrices are the same for the entire frame,
// the lighting and material-source render states are the same for every static-lit draw, and the
// texture stage setup is the same for every textured one. Only the bound texture, the world matrix
// and the draw itself genuinely differ.
//
// This remembers the last value written for each state and drops the call when it would be a
// repeat. Under RTX Remix that is the difference between ~27 and ~3 commands crossing the
// 32-to-64-bit bridge per draw, roughly 19,000 fewer a frame.
//
// WHO IS ALLOWED TO WRITE DEVICE STATE IS THE WHOLE CORRECTNESS ARGUMENT. Every write issued from
// inside agiDX9Rasterizer goes through one of these wrappers, including the ones that put the
// device back at the end of a world run - LeaveWorldState() does not drop the cache, it RECONCILES
// it, by restoring through the same wrappers, so the record stays true across the boundary rather
// than being thrown away at it.
//
// That leaves exactly two kinds of writer to account for, and both are handled explicitly:
//
//   - Code outside the rasterizer that writes the device directly: agiDX9Viewport::Clear, the
//     fullscreen blit in dx9target.cpp, the reflection and ground-environment passes, the
//     -ffperpixel passes, the Pathway B shader bind/unbind and the -ghashcolor tint. Each calls
//     agiDX9InvalidateStateCache() when it is done.
//
//   - IDirect3DDevice9::Reset(), which returns the whole device to its defaults behind everyone's
//     back. agiDX9Context calls agiDX9OnDeviceReset() for it. This is not a theoretical case - the
//     menu/race transition, a window resize and a lost device recovered mid-race all reset.
//
// A cache that has not heard about one of those does not just go stale. Because its whole job is to
// SKIP a call it believes redundant, a stale entry SUPPRESSES the write that would have repaired
// the device, and the wrong state persists until something happens to ask for a different value.
struct agiDX9WorldStateCache
{
    // D3DRS_BLENDOPALPHA is 209, the highest render state D3D9 defines. D3DTSS_CONSTANT is 32.
    static constexpr u32 kRenderStates = 210;
    static constexpr u32 kStageStates = 33;
    static constexpr u32 kStages = 2;

    DWORD RenderState[kRenderStates];
    bool RenderStateKnown[kRenderStates];

    DWORD StageState[kStages][kStageStates];
    bool StageStateKnown[kStages][kStageStates];

    IDirect3DTexture9* Texture[kStages];
    bool TextureKnown[kStages];

    D3DMATRIX World;
    D3DMATRIX View;
    D3DMATRIX Projection;
    bool WorldKnown;
    bool ViewKnown;
    bool ProjectionKnown;

    DWORD Fvf;
    bool FvfKnown;
};

static agiDX9WorldStateCache g_WorldCache {};

void agiDX9InvalidateStateCache()
{
    g_WorldCache = {};
}

// See the long note on the declaration in dx9rsys.h for why a reset the mirrors do not hear about
// is worse than a stale mirror.
void agiDX9OnDeviceReset(IDirect3DDevice9* device)
{
    agiDX9InvalidateStateCache();
    agiDX9InvalidateLightCache();
    agiDX9InvalidateSamplerCache();

    // The engine's own record of what was last sent, which FlushState() compares against and only
    // re-issues on a difference. Reset() poisons it so the next FlushState() re-issues everything
    // rather than trusting a description of the previous device. Same call agiPipeline::BeginAllGfx
    // makes after bringing a pipeline up, and agiDX9Pipeline::EndFrame after the fullscreen blit.
    agiLastState.Reset();

    if (device == nullptr)
        return;

    // D3DSAMP_MAXANISOTROPY is programmed once in BeginGfx() because it never changes for the life
    // of the device - but a Reset() ends that life as far as sampler state is concerned, and the
    // device-lost recovery in agiDX9Context::BeginFrame() resets without any BeginGfx() following
    // it. Put it back from the value BeginGfx() resolved; ApplyTexFilters() still decides per bind
    // whether to actually select D3DTEXF_ANISOTROPIC.
    if (g_MaxAnisotropy > 1)
    {
        for (DWORD stage = 0; stage < 8; ++stage)
            device->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, g_MaxAnisotropy);
    }
}

static void WorldSetRenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value)
{
    const u32 index = static_cast<u32>(state);

    if (index < agiDX9WorldStateCache::kRenderStates)
    {
        if (agiDX9StateCacheEnabled() && g_WorldCache.RenderStateKnown[index] &&
            (g_WorldCache.RenderState[index] == value))
            return;

        g_WorldCache.RenderState[index] = value;
        g_WorldCache.RenderStateKnown[index] = true;
    }

    device->SetRenderState(state, value);
}

// The last per-draw device call that was still issued unconditionally.
//
// Both submission paths bind stage 0 on every draw - MeshWorld() from agiCurState, DrawMesh() from
// current_texture_ - and the engine sorts by texture, so consecutive draws usually want the binding
// that is already there. Every one of those is a command across the Remix 32-to-64-bit bridge for
// no change in state, which is the same cost this cache was built to remove from the render states.
//
// Bound by the same scope rule as the rest of the cache: anything that writes a texture straight to
// the device does so inside a world run and drops the cache afterwards, and a texture being released
// forgets its slot (agiDX9ForgetTexture) so a recycled allocation at the same address cannot be
// mistaken for the binding that is already live.
static void WorldSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DTexture9* texture)
{
    if (stage < agiDX9WorldStateCache::kStages)
    {
        if (agiDX9StateCacheEnabled() && g_WorldCache.TextureKnown[stage] && (g_WorldCache.Texture[stage] == texture))
            return;

        g_WorldCache.Texture[stage] = texture;
        g_WorldCache.TextureKnown[stage] = true;
    }

    device->SetTexture(stage, texture);
}

void agiDX9ForgetTexture(IDirect3DTexture9* texture)
{
    for (u32 stage = 0; stage < agiDX9WorldStateCache::kStages; ++stage)
    {
        if (g_WorldCache.TextureKnown[stage] && (g_WorldCache.Texture[stage] == texture))
            g_WorldCache.TextureKnown[stage] = false;
    }
}

static void WorldSetTextureStageState(IDirect3DDevice9* device, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    const u32 index = static_cast<u32>(type);

    if ((stage < agiDX9WorldStateCache::kStages) && (index < agiDX9WorldStateCache::kStageStates))
    {
        if (agiDX9StateCacheEnabled() && g_WorldCache.StageStateKnown[stage][index] &&
            (g_WorldCache.StageState[stage][index] == value))
            return;

        g_WorldCache.StageState[stage][index] = value;
        g_WorldCache.StageStateKnown[stage][index] = true;
    }

    device->SetTextureStageState(stage, type, value);
}

// Only the three transforms MeshWorld sets every draw. D3DTS_WORLDMATRIX(1..7) is deliberately not
// cached: it is written only by a skinned submission, whose palette differs every time anyway.
static void WorldSetTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX& matrix)
{
    D3DMATRIX* cached = nullptr;
    bool* known = nullptr;

    // Compared as values rather than switched on. D3DTS_WORLD is D3DTS_WORLDMATRIX(0), which is a
    // cast of 256 rather than an enumerator of _D3DTRANSFORMSTATETYPE, so a case label for it is
    // C4063 - fatal here under /W4 /WX.
    const u32 index = static_cast<u32>(state);

    if (index == static_cast<u32>(D3DTS_WORLD))
    {
        cached = &g_WorldCache.World;
        known = &g_WorldCache.WorldKnown;
    }
    else if (index == static_cast<u32>(D3DTS_VIEW))
    {
        cached = &g_WorldCache.View;
        known = &g_WorldCache.ViewKnown;
    }
    else if (index == static_cast<u32>(D3DTS_PROJECTION))
    {
        cached = &g_WorldCache.Projection;
        known = &g_WorldCache.ProjectionKnown;
    }

    if (cached)
    {
        if (agiDX9StateCacheEnabled() && *known && (std::memcmp(cached, &matrix, sizeof(matrix)) == 0))
            return;

        *cached = matrix;
        *known = true;
    }

    device->SetTransform(state, &matrix);
}

static void WorldSetFVF(IDirect3DDevice9* device, DWORD fvf)
{
    if (agiDX9StateCacheEnabled() && g_WorldCache.FvfKnown && (g_WorldCache.Fvf == fvf))
        return;

    g_WorldCache.Fvf = fvf;
    g_WorldCache.FvfKnown = true;

    device->SetFVF(fvf);
}

i32 agiDX9Rasterizer::BeginGfx()
{
    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    // A fresh or reset device holds none of what the mirrors below remember. See
    // agiDX9InvalidateLightCache() and the world-state note on LeaveWorldState().
    agiDX9InvalidateLightCache();
    world_state_active_ = false;

    // These never change for our purposes - our vertex format never carries per-vertex specular,
    // and lighting is only used by the (separate) world-space path.
    WorldSetRenderState(device, D3DRS_LIGHTING, FALSE);
    WorldSetRenderState(device, D3DRS_SPECULARENABLE, FALSE);
    WorldSetRenderState(device, D3DRS_DITHERENABLE, TRUE);

    // Anisotropy. Programmed once here rather than at every texture bind: D3DSAMP_MAXANISOTROPY is
    // sampler state, not texture state, and the value never changes for the life of the device -
    // only whether a given bind SELECTS D3DTEXF_ANISOTROPIC does, and that is ApplyTexFilters' job.
    g_MaxAnisotropy = 1;

    D3DCAPS9 caps {};

    if (SUCCEEDED(device->GetDeviceCaps(&caps)))
    {
        // Both halves are required and they are separate caps bits. A device can report a
        // MaxAnisotropy above 1 and still not support the minification filter that uses it, and
        // asking for a filter the device does not have makes the draw fail rather than degrade.
        const bool supported = (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) != 0;

        if (supported && (caps.MaxAnisotropy > 1))
        {
            const DWORD wanted = static_cast<DWORD>(std::max(1, PARAM_aniso.get_or(16)));

            g_MaxAnisotropy = std::min(wanted, caps.MaxAnisotropy);
        }
    }

    if (g_MaxAnisotropy > 1)
    {
        for (DWORD stage = 0; stage < 8; ++stage)
            device->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, g_MaxAnisotropy);

        Displayf("DX9: anisotropic filtering %ux (device max %u)", g_MaxAnisotropy, caps.MaxAnisotropy);
    }

    current_texture_ = nullptr;
    tex_env_ = agiTexEnv::Disable;

    return AGI_ERROR_SUCCESS;
}

void agiDX9Rasterizer::EndGfx()
{
    current_texture_ = nullptr;

    // The device is going away or being reset; nothing it held is knowable afterwards, and there is
    // no point restoring state onto it either.
    agiDX9InvalidateLightCache();
    world_state_active_ = false;
}

void agiDX9Rasterizer::BeginGroup()
{}

void agiDX9Rasterizer::EndGroup()
{
    ImmDraw();

    // End of a render group is a boundary the world path must not straddle - see LeaveWorldState().
    LeaveWorldState();

    ImmVtxBase = nullptr;
    ImmVtxCount = 0;
}

void agiDX9Rasterizer::Verts(agiVtxType type, agiVtx* vertices, i32 vertex_count)
{
    ArAssert(type == agiVtxType::Screen, "Invalid Vertex Type");

    ImmVtxBase = vertices;
    ImmVtxCount = vertex_count;
}

void agiDX9Rasterizer::Points(
    [[maybe_unused]] agiVtxType type, [[maybe_unused]] agiVtx* vertices, [[maybe_unused]] i32 vertex_count)
{}

void agiDX9Rasterizer::SetVertCount(i32 vertex_count)
{
    ImmVtxCount = vertex_count;
}

void agiDX9Rasterizer::Triangle(i32 i0, i32 i1, i32 i2)
{
    ++STATS.Tris;

    u16* indices = ImmAddIndices(D3DPT_TRIANGLELIST, 3);
    indices[0] = static_cast<u16>(i0);
    indices[1] = static_cast<u16>(i1);
    indices[2] = static_cast<u16>(i2);
}

void agiDX9Rasterizer::Line(i32 i0, i32 i1)
{
    ++STATS.Lines;

    u16* indices = ImmAddIndices(D3DPT_LINELIST, 2);
    indices[0] = static_cast<u16>(i0);
    indices[1] = static_cast<u16>(i1);
}

u16* agiDX9Rasterizer::ImmAddIndices(u32 prim_type, u16 count)
{
    if ((prim_type != ImmPrimType) || (ImmIdxCount + count > ARTS_SIZE(ImmIdxBuffer)))
    {
        ImmDraw();
        ImmPrimType = prim_type;
    }

    u16* result = &ImmIdxBuffer[ImmIdxCount];
    ImmIdxCount += count;
    return result;
}

void agiDX9Rasterizer::ImmDraw()
{
    if (ImmVtxCount && ImmIdxCount)
        DrawMesh(ImmPrimType, ImmVtxBase, ImmVtxCount, ImmIdxBuffer, std::exchange(ImmIdxCount, 0));
}

void agiDX9Rasterizer::Card(i32 v0, i32 v1)
{
    ++STATS.Cards;

    // Never implemented in any renderer (agigl's Card() is an identical no-op stub) - this is
    // our best evidence-based reconstruction: Card sits between Line(i0,i1) (2 vertex indices,
    // referencing the buffer set by Verts()) and Mesh() in the vtable, so it very likely draws a
    // simple screen-space sprite/billboard quad from 2 DIAGONAL CORNERS rather than needing all 4
    // corners spelled out - a common shortcut for flat sprites (light flares, checkpoint
    // billboards) where all 4 corners share one texture/depth. The 2 unlisted corners mix each
    // given corner's position axis with the OTHER corner's texture coordinate.
    if (!ImmVtxBase)
        return;

    // Flush any pending immediate-mode batch first to preserve draw order - this needs its own
    // small vertex buffer (2 synthesized corners) that doesn't exist in ImmVtxBase.
    ImmDraw();

    const agiScreenVtx& a = ImmVtxBase[v0].Screen;
    const agiScreenVtx& b = ImmVtxBase[v1].Screen;

    agiScreenVtx quad[4];
    quad[0] = a;
    quad[1] = a;
    quad[1].x = b.x;
    quad[1].tu = b.tu;
    quad[2] = b;
    quad[3] = b;
    quad[3].x = a.x;
    quad[3].tu = a.tu;

    u16 indices[6] {0, 1, 2, 0, 2, 3};

    STATS.Tris += 2;

    DrawMesh(D3DPT_TRIANGLELIST, reinterpret_cast<agiVtx*>(quad), 4, indices, 6);
}

void agiDX9Rasterizer::Mesh(agiVtxType type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count)
{
    ArAssert(type == agiVtxType::Screen, "Invalid Vertex Type");

    STATS.Tris += index_count / 3;

    DrawMesh(D3DPT_TRIANGLELIST, vertices, vertex_count, indices, index_count);
}

static D3DCMPFUNC ToD3DCmpFunc(agiCmpFunc func)
{
    switch (func)
    {
        case agiCmpFunc::Never: return D3DCMP_NEVER;
        case agiCmpFunc::Less: return D3DCMP_LESS;
        case agiCmpFunc::Equal: return D3DCMP_EQUAL;
        case agiCmpFunc::LessEqual: return D3DCMP_LESSEQUAL;
        case agiCmpFunc::Greater: return D3DCMP_GREATER;
        case agiCmpFunc::Notequal: return D3DCMP_NOTEQUAL;
        case agiCmpFunc::GreaterEqual: return D3DCMP_GREATEREQUAL;
        case agiCmpFunc::Always: return D3DCMP_ALWAYS;
    }

    return D3DCMP_ALWAYS;
}

// Shared by FlushState() and MeshWorld(). Applies the sampler filters implied by `tex_filter`,
// plus (inside SetFilters) the texture's own wrap/clamp addressing.
static void ApplyTexFilters(agiDX9TexDef* texture, agiTexFilter tex_filter)
{
    if (texture->Tex.DisableMipMaps() && tex_filter > agiTexFilter::Bilinear)
        tex_filter = agiTexFilter::Bilinear;

    D3DTEXTUREFILTERTYPE min_filter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE mag_filter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE mip_filter = D3DTEXF_NONE;

    switch (tex_filter)
    {
        case agiTexFilter::Trilinear:
            min_filter = D3DTEXF_LINEAR;
            mag_filter = D3DTEXF_LINEAR;
            mip_filter = D3DTEXF_LINEAR;
            break;

        case agiTexFilter::Bilinear:
            min_filter = D3DTEXF_LINEAR;
            mag_filter = D3DTEXF_LINEAR;
            mip_filter = D3DTEXF_POINT;
            break;

        case agiTexFilter::Point:
            min_filter = D3DTEXF_POINT;
            mag_filter = D3DTEXF_POINT;
            mip_filter = D3DTEXF_POINT;
            break;
    }

    // Anisotropy replaces the MINIFICATION filter only, which is the whole point: magnification is
    // a surface closer than one texel per pixel, where there is no anisotropy to resolve and the
    // extra taps buy nothing. Point-filtered textures are left alone entirely - that mode exists to
    // be exact, and this engine uses it for content that is meant to look like it did in 1999.
    if ((g_MaxAnisotropy > 1) && (min_filter == D3DTEXF_LINEAR))
        min_filter = D3DTEXF_ANISOTROPIC;

    texture->SetFilters(min_filter, mag_filter, mip_filter);
}

// Shared by FlushState() and MeshWorld(): MeshWorld() sets stage/render state directly (it has to -
// see the DrawMode note there) and must be able to put the device back exactly as agiLastState
// describes it, so both places have to agree on what each state value means.
static void ApplyTexEnv(IDirect3DDevice9* device, agiTexEnv tex_env)
{
    switch (tex_env)
    {
        case agiTexEnv::Disable:
            WorldSetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            WorldSetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            WorldSetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            WorldSetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            break;

        case agiTexEnv::Replace:
            WorldSetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            WorldSetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            WorldSetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            WorldSetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            break;

        case agiTexEnv::Modulate:
            WorldSetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            WorldSetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            WorldSetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            WorldSetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            WorldSetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            WorldSetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            break;
    }
}

// Shared by FlushState() and MeshWorld(), for the same reason ApplyTexEnv() is: the native
// transform path bypasses agiTexSorter::DoTexture() (still closed assembly, game.asm), which is
// what binds per-texture material state on the CPU path, so MeshWorld() has to program the blend
// mode itself rather than inherit whatever the last CPU-path draw happened to leave behind.
static void ApplyBlendSet(IDirect3DDevice9* device, agiBlendSet blend_set)
{
    D3DBLEND blend_s = D3DBLEND_ONE;
    D3DBLEND blend_d = D3DBLEND_ZERO;

    switch (blend_set)
    {
        case agiBlendSet::SrcAlpha_InvSrcAlpha:
            blend_s = D3DBLEND_SRCALPHA;
            blend_d = D3DBLEND_INVSRCALPHA;
            break;

        case agiBlendSet::SrcAlpha_One:
            blend_s = D3DBLEND_SRCALPHA;
            blend_d = D3DBLEND_ONE;
            break;

        case agiBlendSet::Zero_SrcAlpha:
            blend_s = D3DBLEND_ZERO;
            blend_d = D3DBLEND_SRCALPHA;
            break;

        case agiBlendSet::Zero_SrcColor:
            blend_s = D3DBLEND_ZERO;
            blend_d = D3DBLEND_SRCCOLOR;
            break;

        case agiBlendSet::One_One:
            blend_s = D3DBLEND_ONE;
            blend_d = D3DBLEND_ONE;
            break;

        default: Quitf("bad blend mode"); break;
    }

    WorldSetRenderState(device, D3DRS_SRCBLEND, blend_s);
    WorldSetRenderState(device, D3DRS_DESTBLEND, blend_d);
}

static D3DCULL ToD3DCull(agiCullMode cull_mode)
{
    switch (cull_mode)
    {
        case agiCullMode::CW: return D3DCULL_CW;
        case agiCullMode::CCW: return D3DCULL_CCW;
        default: return D3DCULL_NONE;
    }
}

// Same mapping, but with the two culling senses exchanged - for the hardware-transform path, whose
// triangles reach the rasterizer with the opposite orientation from the CPU pretransform path's.
//
// The cause is the Z reflection in MeshWorld()'s `view_zflip`. That negates the Z *column* of the
// view matrix (each row's .z member), which is what puts view-space depth into the positive-forward
// convention BuildProjectionMatrix() is tuned for. A single-axis negation is a reflection: its
// determinant is negative, so it reverses the orientation of every triangle passing through it. The
// CPU path reaches the same depth convention differently - agiMeshSet::M negates the Z *row*, and
// its screen mapping (InitViewport: HalfWidth = +w/2, HalfHeight = -h/2) contributes a second
// reflection through the Y flip - so the two paths end up with opposite screen-space winding even
// though they produce identical pixel positions.
//
// Confirmed directly: feeding agiCurState's cull mode through unmodified culled *front* faces, and
// the frame came back showing the inside of the city - back faces of building shells, the road
// missing entirely, and the FINISH banner rendering its texture mirrored. Exchanging the senses
// renders correctly. This is the same compensation agigl performs with its `flip_winding_` flag.
static D3DCULL ToD3DCullFlipped(agiCullMode cull_mode)
{
    switch (cull_mode)
    {
        case agiCullMode::CW: return D3DCULL_CCW;
        case agiCullMode::CCW: return D3DCULL_CW;
        default: return D3DCULL_NONE;
    }
}

void agiDX9Rasterizer::FlushState()
{
    if (!agiCurState.IsTouched())
        return;

    ARTS_UTIMED(agiStateChanges);
    ++STATS.StateChanges;

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    agiDX9TexDef* texture =
        (agiCurState.GetDrawMode() == agiDrawTextured) ? static_cast<agiDX9TexDef*>(agiCurState.GetTexture()) : nullptr;

    agiTexFilter tex_filter = agiCurState.GetTexFilter();

    if (texture != agiLastState.Texture)
    {
        if (texture)
        {
            IDirect3DTexture9* handle = texture->GetHandle();

            // NOT-YET-RESIDENT IS A TRANSIENT, NOT A BINDING - and recording it as one latches the
            // texture black for the rest of the process.
            //
            // agiDX9TexDef::GetHandle() legitimately returns null while a texture is still paging
            // in: it kicks off PageInSurface() and answers null for that call. This used to write
            // agiLastState.Texture = texture *before* asking, so a null answer left the pair in an
            // impossible state - agiLastState saying "this texture is bound", current_texture_
            // saying "nothing is bound". DrawMesh() then dropped the draw on its null-texture guard,
            // and because agiLastState now matched, EVERY following flush took the `texture ==
            // agiLastState.Texture` fast path and never called GetHandle() again. One unlucky frame
            // and that bitmap never drew again.
            //
            // That is the black menu art. It hits large background bitmaps rather than small widget
            // art because they are the ones still paging when first bound, and it explains why the
            // art "came back" whenever something else was drawn between two bitmaps - an
            // intervening texture moved agiLastState.Texture, so the next bind was forced to
            // re-resolve. The world path never showed it because MeshWorld() calls GetHandle()
            // afresh on every draw and so heals itself.
            //
            // Recording null instead means the next flush sees a difference again and retries, which
            // is what "not ready yet" should do. The draw for this frame is still dropped - that is
            // correct, there is nothing to draw with - but it is dropped once instead of forever.
            agiLastState.Texture = handle ? texture : nullptr;

            if (handle != current_texture_)
            {
                current_texture_ = handle;

                ++STATS.TextureChanges;
                STATS.TxlsXrfd += texture->SurfaceSize;

                if (current_texture_)
                {
                    ApplyTexFilters(texture, tex_filter);
                }
            }
        }
        else
        {
            agiLastState.Texture = nullptr;
            current_texture_ = nullptr;
        }
    }

    if (tex_filter != agiLastState.TexFilter)
    {
        agiLastState.TexFilter = tex_filter;
    }

    bool zenable = agiEnableZBuffer && agiCurState.GetZEnable();

    if (zenable != agiLastState.ZEnable)
    {
        agiLastState.ZEnable = zenable;
        WorldSetRenderState(device, D3DRS_ZENABLE, zenable ? D3DZB_TRUE : D3DZB_FALSE);
        ++STATS.StateChangeCalls;
    }

    if (zenable)
    {
        if (bool zwrite = agiCurState.GetZWrite(); zwrite != agiLastState.ZWrite)
        {
            agiLastState.ZWrite = zwrite;

            WorldSetRenderState(device, D3DRS_ZWRITEENABLE, zwrite ? TRUE : FALSE);
            ++STATS.StateChangeCalls;
        }

        if (agiCmpFunc zfunc = agiCurState.GetZFunc(); zfunc != agiLastState.ZFunc)
        {
            agiLastState.ZFunc = zfunc;

            WorldSetRenderState(device, D3DRS_ZFUNC, ToD3DCmpFunc(zfunc));
            ++STATS.StateChangeCalls;
        }
    }

    if (bool smooth_shading = agiCurState.GetSmoothShading(); smooth_shading != agiLastState.SmoothShading)
    {
        agiLastState.SmoothShading = smooth_shading;

        WorldSetRenderState(device, D3DRS_SHADEMODE, smooth_shading ? D3DSHADE_GOURAUD : D3DSHADE_FLAT);
        ++STATS.StateChangeCalls;
    }

    if (agiDrawMode draw_mode = agiCurState.GetDrawMode(); draw_mode != agiLastState.DrawMode)
    {
        agiLastState.DrawMode = draw_mode;

        D3DFILLMODE fill_mode = D3DFILL_SOLID;

        switch (static_cast<agiFillMode>(draw_mode & agiDrawFillMask))
        {
            case agiFillMode::Point: fill_mode = D3DFILL_POINT; break;
            case agiFillMode::Wire: fill_mode = D3DFILL_WIREFRAME; break;
            case agiFillMode::Solid: fill_mode = D3DFILL_SOLID; break;
        }

        WorldSetRenderState(device, D3DRS_FILLMODE, fill_mode);
        ++STATS.StateChangeCalls;
    }

    bool alpha_enable = agiCurState.GetAlphaEnable();

    if (texture)
        alpha_enable |= texture->Tex.HasAlpha() || texture->Tex.UseChromakey();

    u8 alpha_ref = agiCurState.GetAlphaRef();

    if (alpha_enable != agiLastState.AlphaEnable || alpha_ref != agiLastState.AlphaRef)
    {
        agiLastState.AlphaEnable = alpha_enable;
        agiLastState.AlphaRef = alpha_ref;

        WorldSetRenderState(device, D3DRS_ALPHABLENDENABLE, alpha_enable ? TRUE : FALSE);
        WorldSetRenderState(device, D3DRS_ALPHATESTENABLE, alpha_enable ? TRUE : FALSE);
        ++STATS.StateChangeCalls;

        if (alpha_enable)
        {
            WorldSetRenderState(device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
            WorldSetRenderState(device, D3DRS_ALPHAREF, alpha_ref);
            ++STATS.StateChangeCalls;
        }
    }

    if (agiBlendSet blend_set = agiCurState.GetBlendSet(); blend_set != agiLastState.BlendSet)
    {
        agiLastState.BlendSet = blend_set;

        ApplyBlendSet(device, blend_set);
        ++STATS.StateChangeCalls;
    }

    if (agiCullMode cull_mode = agiCurState.GetCullMode(); cull_mode != agiLastState.CullMode)
    {
        agiLastState.CullMode = cull_mode;

        // Our vertices are already in D3D9 screen space (D3DFVF_XYZRHW), so no winding
        // flip is needed here (unlike the OpenGL backend, which has to compensate for
        // OpenGL's opposite screen-space Y axis).
        WorldSetRenderState(device, D3DRS_CULLMODE, agiNoCullEnabled() ? D3DCULL_NONE : ToD3DCull(cull_mode));
        ++STATS.StateChangeCalls;
    }

    if (agiTexEnv tex_env = texture ? agiCurState.GetTexEnv() : agiTexEnv::Disable; tex_env != agiLastState.TexEnv)
    {
        agiLastState.TexEnv = tex_env;

        if (tex_env != tex_env_)
        {
            tex_env_ = tex_env;

            ApplyTexEnv(device, tex_env);
        }

        ++STATS.StateChangeCalls;
    }

    agiFogMode fog_mode = agiCurState.GetFogMode();
    f32 fog_start = agiCurState.GetFogStart();
    f32 fog_end = agiCurState.GetFogEnd();
    f32 fog_density = agiCurState.GetFogDensity();

    if (fog_mode != agiLastState.FogMode || fog_start != agiLastState.FogStart || fog_end != agiLastState.FogEnd ||
        fog_density != agiLastState.FogDensity)
    {
        agiLastState.FogMode = fog_mode;
        agiLastState.FogStart = fog_start;
        agiLastState.FogEnd = fog_end;
        agiLastState.FogDensity = fog_density;

        WorldSetRenderState(device, D3DRS_FOGENABLE, (fog_mode != agiFogMode::None) ? TRUE : FALSE);

        switch (fog_mode)
        {
            case agiFogMode::None: break;

            case agiFogMode::Pixel:
                // Table (per-pixel) fog for pretransformed vertices - interpolates using the
                // vertex's screen-space depth (agiScreenVtx::z), same source as the OpenGL
                // backend's GL_FOG_COORD path.
                WorldSetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
                WorldSetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
                WorldSetRenderState(device, D3DRS_FOGSTART, *reinterpret_cast<const DWORD*>(&fog_start));
                WorldSetRenderState(device, D3DRS_FOGEND, *reinterpret_cast<const DWORD*>(&fog_end));
                break;

            case agiFogMode::Vertex:
                // With both fog modes set to NONE, D3D9 uses the alpha component of the
                // per-vertex specular colour directly as the fog factor - exactly the value
                // already baked into agiScreenVtx::specular by the engine's own fog code.
                WorldSetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
                WorldSetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
                break;
        }
    }

    if (u32 fog_color = agiCurState.GetFogColor(); fog_color != agiLastState.FogColor)
    {
        agiLastState.FogColor = fog_color;

        WorldSetRenderState(device, D3DRS_FOGCOLOR, fog_color & 0x00FFFFFF);
    }

    agiCurState.ClearTouched();
}

// Per-frame census of how geometry actually reaches the device, so "is anything still submitted
// pretransformed?" is answered by counting rather than by reading call sites - most of the engine
// that draws is still closed assembly, so a source audit alone cannot answer it.
// Pretransformed (XYZRHW) draws carry no world-space information and are invisible to RTX Remix
// as 3D geometry, no matter how they look on screen. Dumped by agiDX9Pipeline::EndFrame().
agiDX9SubmitCensus agiDX9Census {};

// -ghash storage. See PARAM_ghash for what this measures and why.
//
// Open addressing with linear probing over a fixed table, because this runs inside the draw loop
// and must not allocate. The table deliberately stops accepting new entries at half full rather
// than degrading into long probe chains - GHashDistinct then stops rising, which is visible in the
// report, so a saturated table reads as saturated instead of as quietly wrong numbers.
struct agiDX9GHashSlot
{
    u64 Hash;
    u32 LastFrame;
    bool Used;
};

static constexpr u32 kGHashSlots = 16384; // power of two
static constexpr u32 kGHashMask = kGHashSlots - 1;

static agiDX9GHashSlot g_GHashTable[kGHashSlots] {};
static u32 g_GHashFrame = 0;
static u32 g_GHashUsed = 0;
static u32 g_GHashWraps = 0;

// Attribution for the two numbers in the report that say something is wrong without saying WHAT.
//
// CHURN(new) and IN-SCENE(3D) are both bare counts, and a count cannot be acted on: "12 in-scene
// screen calls" does not say which surfaces are still CPU-pretransformed, and "118 new hashes" does
// not say whether that is pedestrians, water, or a smoke sprite legitimately cycling its animation
// frames. In both cases the answer is one texture name away, and tallying names is cheap enough to
// sit in the draw path - the populations being counted are small by construction.
struct agiDX9NameTally
{
    static constexpr u32 kSlots = 24;
    static constexpr u32 kNameLen = 32;

    char Names[kSlots][kNameLen];
    u32 Counts[kSlots];
    u32 Used;
    u32 Dropped;
};

static agiDX9NameTally g_GHashChurnBy {};
static agiDX9NameTally g_ScreenInSceneBy {};

// Screen draws thrown away by DrawMesh()'s null-texture guard, named by the texture that was asked
// for. A draw that never reaches the device is invisible in every other count here - the census
// tallies what was submitted - so a bitmap silently failing to appear left no trace at all. This is
// the line to look at first when menu or HUD art is missing: if the missing image is named here, it
// asked for a texture that was not resident, and the answer is in GetHandle()/paging rather than
// anywhere in the draw path.
static agiDX9NameTally g_ScreenDroppedNoTex {};

// Every screen-space draw that DOES reach the device, named by texture, and the companion to the
// tally above: between them, a menu image is either dropped, submitted, or never asked for at all,
// and those are three different bugs with three different places to look.
//
// This is what the dropped-draw tally could not answer on its own. A run of the black-background
// menus produced no dropped draws whatsoever, which proves the guard is not eating them but says
// nothing about whether the background was ever submitted - the census counts triangles, and one
// quad looks like any other. Naming them separates "submitted and rendering black" (a texture,
// blend or stage-state problem) from "never submitted" (the menu code never asked), and only the
// first of those is the renderer's.
//
// Capped at 24 names like the others, so a race - where the HUD, text and minimap all come through
// here - reports the busiest and then a "(+more)".
static agiDX9NameTally g_ScreenDrawsBy {};

static void agiDX9TallyAdd(agiDX9NameTally& tally, const char* name)
{
    if (!name || !*name)
        name = "(untextured)";

    for (u32 i = 0; i < tally.Used; ++i)
    {
        if (std::strcmp(tally.Names[i], name) == 0)
        {
            ++tally.Counts[i];
            return;
        }
    }

    if (tally.Used >= agiDX9NameTally::kSlots)
    {
        ++tally.Dropped;
        return;
    }

    // Copied rather than pointed at. The tally spans 120 frames, and a pipeline teardown inside
    // that window resets the engine's arena and frees every agiTexDef wholesale - the same lifetime
    // trap documented for the glow registry in docs/remix_api_data_sources.md §6. Hand-rolled
    // because strncpy is a /W4 /WX deprecation error under MSVC.
    char* dst = tally.Names[tally.Used];
    u32 i = 0;

    for (; (i + 1) < agiDX9NameTally::kNameLen && name[i]; ++i)
        dst[i] = name[i];

    dst[i] = '\0';

    tally.Counts[tally.Used] = 1;
    ++tally.Used;
}

static void agiDX9TallyDump(const char* label, agiDX9NameTally& tally)
{
    if (!tally.Used)
        return;

    char line[512];
    i32 offset = 0;

    for (u32 i = 0; (i < tally.Used) && (offset >= 0) && (offset < static_cast<i32>(sizeof(line)) - 1); ++i)
    {
        const i32 written = std::snprintf(line + offset, sizeof(line) - static_cast<usize>(offset), "%s%s=%u",
            (i ? ", " : ""), tally.Names[i], tally.Counts[i]);

        if (written < 0)
            break;

        offset += written;
    }

    line[sizeof(line) - 1] = '\0';

    Displayf("%s %s%s", label, line, tally.Dropped ? " (+more)" : "");
}

void agiDX9DumpAttribution()
{
    // Named by texture, because that is the handle everything else here is keyed on - it is what
    // agiTexProp flags hang off, what the Remix material config keys on, and what a person can
    // actually recognise in a capture.
    agiDX9TallyDump("DX9 GHASH CHURN BY TEXTURE:", g_GHashChurnBy);
    agiDX9TallyDump("DX9 IN-SCENE SCREEN DRAWS BY TEXTURE:", g_ScreenInSceneBy);
    agiDX9TallyDump("DX9 SCREEN DRAWS DROPPED, NO TEXTURE:", g_ScreenDroppedNoTex);
    agiDX9TallyDump("DX9 SCREEN DRAWS SUBMITTED BY TEXTURE:", g_ScreenDrawsBy);

    g_GHashChurnBy = {};
    g_ScreenInSceneBy = {};
    g_ScreenDroppedNoTex = {};
    g_ScreenDrawsBy = {};
}

u32 agiDX9GHashTableUsed()
{
    return g_GHashUsed;
}

u32 agiDX9GHashTableCapacity()
{
    return kGHashSlots / 2;
}

u32 agiDX9GHashWraps()
{
    return g_GHashWraps;
}

// FNV-1a. Not a cryptographic choice and not Remix's own function - what is being measured is
// whether the same bytes come back frame after frame, and any decent mixer answers that.
static u64 agiDX9GHashBytes(const void* data, usize size, u64 hash)
{
    const u8* bytes = static_cast<const u8*>(data);

    for (usize i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }

    return hash;
}

// Hashes one world draw the way Remix would, and classifies it as new or returning.
//
// Inputs mirror the Remix key: vertex bytes for the drawn range, index bytes, stride, vertex count,
// primitive type. Explicitly NOT the transforms - a mesh drawn in two places is ONE mesh to Remix,
// and folding the world matrix in here would invent churn that Remix does not see.
static u64 agiDX9GHashRecord(
    const agiWorldVtx* vertices, i32 vertex_count, const u16* indices, i32 index_count, agiDX9TexDef* texture)
{
    if (!PARAM_ghash.get_or(false) && !PARAM_ghashcolor.get_or(false))
        return 0;

    if (!vertices || !indices || (vertex_count <= 0) || (index_count <= 0))
        return 0;

    u64 hash = 0xCBF29CE484222325ull;

    // POSITION AND TEXCOORD ONLY - deliberately not the whole vertex.
    //
    // Remix's HashComponents (rtx_hashing.h) are VertexPosition, VertexTexcoord, Indices,
    // GeometryDescriptor, VertexLayout and VertexShader. Normals and vertex COLOUR are not in the
    // set, and colour is the one that matters here: vehicles are lit by the dynamic rig, so their
    // per-vertex colours are recomputed every frame. Hashing the whole 0x24-byte agiWorldVtx made
    // every car strobe under -ghashcolor even parked and undamaged, which is an artefact of this
    // diagnostic rather than anything Remix would see.
    //
    // Pedestrians are a different case and will still flicker: they are genuinely CPU-skinned, so
    // their POSITIONS change per frame, and Remix flickers on software-animated meshes too.
    for (i32 i = 0; i < vertex_count; ++i)
    {
        hash = agiDX9GHashBytes(&vertices[i].pos, sizeof(vertices[i].pos), hash);
        hash = agiDX9GHashBytes(&vertices[i].tu, sizeof(f32) * 2, hash);
    }

    hash = agiDX9GHashBytes(indices, static_cast<usize>(index_count) * sizeof(u16), hash);

    const u32 descriptor[3] {static_cast<u32>(sizeof(agiWorldVtx)), static_cast<u32>(vertex_count), D3DPT_TRIANGLELIST};

    hash = agiDX9GHashBytes(descriptor, sizeof(descriptor), hash);

    ++agiDX9Census.GHashDraws;

    const u32 start = static_cast<u32>(hash) & kGHashMask;

    for (u32 probe = 0; probe < 64; ++probe)
    {
        agiDX9GHashSlot& slot = g_GHashTable[(start + probe) & kGHashMask];

        if (!slot.Used)
        {
            // Table full.
            //
            // This used to `return hash` here, which turned the one number the diagnostic exists to
            // produce into a lie. With no free slots, every genuinely new hash returned early
            // WITHOUT being counted, so CHURN(new) read a steady zero however much churn there was
            // - and a long session reaches that state on its own, because a single CPU-skinned
            // pedestrian mints a fresh hash every frame. "CHURN=0" twenty minutes into a drive
            // therefore meant "the table gave up", not "the geometry is stable", and the two are
            // indistinguishable in the report. Measured: a log full of CHURN=0 alongside visibly
            // strobing pedestrians and water under -ghashcolor.
            //
            // Clearing and starting over keeps it honest. Churn reappears in the very next frame's
            // count, and the wrap counter in the report says how often it is happening - a scene
            // with no churn never wraps at all, so wraps>0 is itself the signal. The used/capacity
            // figure next to it shows how close the table is to the next wrap.
            if (g_GHashUsed >= (kGHashSlots / 2))
            {
                for (u32 i = 0; i < kGHashSlots; ++i)
                    g_GHashTable[i] = {};

                ++g_GHashWraps;

                // The table is empty now, so the first probe position is free by construction.
                agiDX9GHashSlot& fresh = g_GHashTable[start];

                fresh.Used = true;
                fresh.Hash = hash;
                fresh.LastFrame = g_GHashFrame;

                g_GHashUsed = 1;
            }
            else
            {
                slot.Used = true;
                slot.Hash = hash;
                slot.LastFrame = g_GHashFrame;

                ++g_GHashUsed;
            }

            ++agiDX9Census.GHashNew;
            ++agiDX9Census.GHashDistinct;

            agiDX9TallyAdd(g_GHashChurnBy, texture ? texture->Tex.Name : nullptr);

            return hash;
        }

        if (slot.Hash == hash)
        {
            // Count each distinct hash once per frame, however many times it is drawn - the same
            // mesh submitted twice is one identity to Remix, not two.
            if (slot.LastFrame != g_GHashFrame)
            {
                slot.LastFrame = g_GHashFrame;

                ++agiDX9Census.GHashDistinct;
                ++agiDX9Census.GHashStable;
            }

            return hash;
        }
    }

    return hash;
}

// Hash -> flat colour, the same idea as Remix's Debug View -> Geometry Hash: a mesh whose hash is
// stable holds one steady colour, and a mesh being rehashed every frame strobes. That is the
// spatial question the numeric report cannot answer - WHICH mesh is unstable, not how many.
//
// Per NVIDIA's own guidance for that view, strobing is expected for particles and for CPU-skinned
// ("software animation") meshes, while GPU-skinned geometry stays stable. MM1 animates pedestrians
// on the CPU, so the prediction here is a solid city with strobing pedestrians.
//
// The low 24 bits become RGB, floored away from black so nothing reads as an unlit surface.
static u32 agiDX9GHashColor(u64 hash)
{
    const u32 r = 64 + static_cast<u32>((hash >> 0) & 0x7F);
    const u32 g = 64 + static_cast<u32>((hash >> 8) & 0x7F);
    const u32 b = 64 + static_cast<u32>((hash >> 16) & 0x7F);

    return D3DCOLOR_XRGB(r, g, b);
}

void agiDX9GHashNextFrame()
{
    ++g_GHashFrame;
}

// Fog must be OFF for additively-composited AlphaGlow content, and the engine says so itself.
//
// FirstPass_* (game.asm, e.g. ~324982) does exactly this: for a texture with agiTexProp::AlphaGlow,
// when DisableFogOnAlphaGlow is set, it zeroes agiCurState+0x17 - which is state_+0x13, i.e.
// agiRendStateStruct::FogMode (agi/rsys.h: agiRendState is a b32 touched_ followed by the struct).
// Neither of this backend's draw paths reproduced that, so glows were being fogged.
//
// The visible result is the reported one: a highlighted rectangle around every flare. A glow is
// blended One_One, and its texture background is black precisely so that black contributes nothing
// under an additive blend. Fog breaks that invariant - it computes lerp(src, fogColor, f) *before*
// the blend, so the background stops being black and becomes fogColor*f, which is then added to the
// framebuffer across the whole quad. The flare itself hides it in the middle; at the edges, where
// there is nothing but background, the quad's own rectangle shows up as a glowing box. It gets
// worse with distance (f grows) and worse again where several particle quads overlap, because each
// one adds its own rectangle - which is why it reads as particles and fog interacting.
static bool IsAdditiveGlow(agiTexDef* texture)
{
    return texture && (static_cast<agiDX9TexDef*>(texture)->Tex.Props & agiTexProp::AlphaGlow);
}

void agiDX9Rasterizer::DrawMesh(u32 prim_type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count)
{
    if (!IsAppActive() || (vertex_count == 0) || (index_count == 0))
        return;

    // Before FlushState(), not after: FlushState() only re-issues state agiCurState has actually
    // changed, so it cannot undo what MeshWorld() left on the device. Putting the device back to
    // what agiLastState describes has to happen first, which is exactly what this does.
    LeaveWorldState();

    FlushState();

    if ((current_texture_ == nullptr) && (tex_env_ != agiTexEnv::Disable))
    {
        // Name what was thrown away. See g_ScreenDroppedNoTex - a dropped draw is invisible to every
        // other counter in the census, so missing art used to leave no evidence anywhere.
        agiDX9TexDef* wanted = static_cast<agiDX9TexDef*>(agiCurState.GetTexture());

        agiDX9TallyAdd(g_ScreenDroppedNoTex, wanted ? wanted->Tex.Name : nullptr);

        return;
    }

    ARTS_UTIMED(agiRasterization);
    ++STATS.GeomCalls;

    // Name what does reach the device. See g_ScreenDrawsBy - the counterpart to the dropped tally
    // above, and the only way to tell a menu image that renders black from one that was never
    // submitted at all.
    {
        agiDX9TexDef* submitted = static_cast<agiDX9TexDef*>(agiCurState.GetTexture());

        agiDX9TallyAdd(g_ScreenDrawsBy, submitted ? submitted->Tex.Name : nullptr);
    }

    if (prim_type == D3DPT_LINELIST)
    {
        ++agiDX9Census.ScreenLineCalls;
        agiDX9Census.ScreenLines += index_count / 2;
    }
    else
    {
        ++agiDX9Census.ScreenCalls;
        agiDX9Census.ScreenTris += index_count / 3;

        if (Pipe()->IsInScene())
        {
            ++agiDX9Census.ScreenCallsInScene;
            agiDX9Census.ScreenTrisInScene += index_count / 3;

            // Name them. These are real 3D content going out CPU-pretransformed - invisible to
            // Remix as geometry, and the reason some surfaces take no tint under -ghashcolor while
            // everything around them does: the tint is applied in MeshWorld, which these never
            // reach. The count alone cannot say which surfaces they are, and most of what still
            // draws is closed assembly, so a source audit cannot either. The texture name can.
            agiDX9TexDef* screen_tex = static_cast<agiDX9TexDef*>(agiCurState.GetTexture());

            agiDX9TallyAdd(g_ScreenInSceneBy, screen_tex ? screen_tex->Tex.Name : nullptr);
        }
    }

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    WorldSetTexture(device, 0, current_texture_);
    WorldSetFVF(device, kScreenVtxFVF);

    // See IsAdditiveGlow(). Only touched for glow draws, so ordinary geometry pays nothing.
    const bool glow = IsAdditiveGlow(agiCurState.GetTexture());

    if (glow)
        WorldSetRenderState(device, D3DRS_FOGENABLE, FALSE);

    i32 primitive_count = (prim_type == D3DPT_LINELIST) ? (index_count / 2) : (index_count / 3);

    // -ghashcolor marks in-scene CPU-pretransformed geometry FLAT MAGENTA.
    //
    // The debug view's whole premise is "a stable mesh holds one colour", and a surface that is
    // still on this path takes no tint at all - it cannot, because the tint is applied in
    // MeshWorld() and this geometry never goes there. So it renders with its ordinary texture and
    // reads, wrongly, as though it were simply not participating. That is the reported "some
    // building faces show their original textures", and the two states it could mean - stable, or
    // absent from the world path entirely - look identical on screen while being opposite problems.
    //
    // Magenta because nothing in this game is magenta, and because a hash colour would imply this
    // draw has an identity Remix could key on. It does not: pretransformed vertices carry no
    // world-space information, so anything painted magenta here is geometry Remix cannot see at all.
    // Pair it with the IN-SCENE SCREEN DRAWS BY TEXTURE line, which names the same draws.
    const bool mark_screen = PARAM_ghashcolor.get_or(false) && Pipe()->IsInScene();

    if (mark_screen)
    {
        WorldSetRenderState(device, D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(255, 0, 255));
        WorldSetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        WorldSetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    }

    // Logical pixels -> backbuffer pixels. See the PRESENTATION TRANSFORM note in dx9pipe.h.
    //
    // This is the one place every CPU-pretransformed submission passes through - Mesh(), Card() and
    // ImmDraw() all end here - so it is the only place the mapping has to be applied for screen
    // content. It has to be applied to the vertices themselves: an agiScreenVtx is XYZRHW, and D3D9
    // gives pretransformed geometry the viewport's *origin* and its clip rectangle but never its
    // scale, so nothing about the device viewport can move these.
    //
    // Working on a copy rather than in place because the caller owns this memory and keeps using it:
    // ImmVtxBase is one vertex array that ImmDraw() may submit several times as ImmAddIndices()
    // flushes on a primitive-type change or a full index buffer, and transforming it in place would
    // scale the second flush twice. ARTS_ALLOCA for the copy, for the reason
    // agiMeshModel::ModelDrawLit documents - the engine allocator must not be called from inside a
    // draw - and because the ceiling is BigVtxSize (16384), which the world path already alloca's
    // more than (meshrend.cpp ~2018).
    //
    // Untouched, not merely unscaled, when the transform is the identity: that is the ordinary
    // windowed case and fullscreen at the desktop resolution, and it is most of the time.
    const agiScreenVtx* draw_verts = reinterpret_cast<const agiScreenVtx*>(vertices);

    if (Pipe()->ScreenScaleActive())
    {
        const f32 scale_x = Pipe()->ScreenScaleX();
        const f32 scale_y = Pipe()->ScreenScaleY();
        const f32 offset_x = static_cast<f32>(Pipe()->ScreenOffsetX());
        const f32 offset_y = static_cast<f32>(Pipe()->ScreenOffsetY());

        agiScreenVtx* scaled = ARTS_ALLOCA(agiScreenVtx, vertex_count);

        for (i32 i = 0; i < vertex_count; ++i)
        {
            scaled[i] = draw_verts[i];
            scaled[i].x = draw_verts[i].x * scale_x + offset_x;
            scaled[i].y = draw_verts[i].y * scale_y + offset_y;
        }

        draw_verts = scaled;
    }

    device->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(prim_type), 0, vertex_count, primitive_count, indices,
        D3DFMT_INDEX16, draw_verts, sizeof(agiScreenVtx));

    // Put the stage back, or every following screen draw inherits TFACTOR - including the HUD, which
    // is drawn after EndScene and would go solid magenta with it.
    if (mark_screen)
        ApplyTexEnv(device, tex_env_);

    // Restore to whatever FlushState() believes it left on the device. It caches fog in
    // agiLastState and only re-issues on a change, so leaving fog off here would silently unfog
    // every following draw that happened not to change fog state.
    if (glow)
    {
        WorldSetRenderState(device, D3DRS_FOGENABLE, (agiLastState.FogMode != agiFogMode::None) ? TRUE : FALSE);
    }
}

// Matrix34 (vector7/matrix34.h) is already row-vector, D3D9-shaped (m0/m1/m2 = X/Y/Z basis
// rows, m3 = translation row) - this is a direct field embed, no transposition needed.
static D3DMATRIX ToD3DMatrix(const Matrix34& m)
{
    D3DMATRIX result;

    result._11 = m.m0.x;
    result._12 = m.m0.y;
    result._13 = m.m0.z;
    result._14 = 0.0f;

    result._21 = m.m1.x;
    result._22 = m.m1.y;
    result._23 = m.m1.z;
    result._24 = 0.0f;

    result._31 = m.m2.x;
    result._32 = m.m2.y;
    result._33 = m.m2.z;
    result._34 = 0.0f;

    result._41 = m.m3.x;
    result._42 = m.m3.y;
    result._43 = m.m3.z;
    result._44 = 1.0f;

    return result;
}

// Built directly from agiViewParameters' already-computed projection coefficients (no need to
// touch the still-imported agiViewParameters::Perspective() that derives them). Mathematically
// equivalent to the CPU M-multiply + ToScreen() every other renderer relies on: with row-vectors
// v' = v * World * View * Projection, the third row/fourth column here reproduce
// `z_clip = view_z * ProjZZ + ProjZW` and `w_clip = view_z` exactly as agiMeshSet::Transform() does.
//
// The CPU path's ToScreen() doesn't stop at `ProjZZ + ProjZW/view_z` - it additionally remaps
// that value through `* agiMeshSet::DepthScale + agiMeshSet::DepthOffset` before writing
// agiScreenVtx::z. That remap turns the engine's OpenGL-style [-1, 1] NDC depth into D3D9's
// [0, 1] depth range, so the *halving* part of it has to be here: skipping it would leave
// clip-space Z in the wrong range for D3D9's hardware near/far clipping, causing incorrect
// clipping of large surfaces (roads, building facades) that span a wide depth range.
//
// THE HALVING IS ALL THAT MAY BE HERE. This function deliberately hardcodes 0.5/0.5 rather than
// reading agiMeshSet::DepthScale/DepthOffset, and agiDX9Pipeline::BeginFrame() forces those two
// globals to 0.5/0.5 so the CPU path agrees. Both halves of that are one fix, and it is the fix
// for "RTX Remix path traces the car selector but not the race".
//
// The engine does not actually keep DepthScale at 0.5. mmCullCity's constructor - which runs when
// a city loads, and never in the menus or the showroom - lowers agiMeshSet::DepthScale to 0.495 or
// 0.499 (game.asm ~175726), and mmCullCity::Cull() additionally subtracts ShadowZBias (0.005) from
// DepthOffset for the duration of the asPortalWeb pass (~180783). That is a guard band: it insets
// the written depth to roughly [0.005, 0.995] so a software rasteriser never lands exactly on the
// endpoints. Harmless as a per-vertex remap. Fatal as a projection matrix, because a projection
// matrix is not a remap - it IS the frustum, and the numbers must describe a real one:
//
//     _33 = -ProjZZ * s + o   _43 = ProjZW * s   _34 = 1   _44 = 0
//     z_ndc = _33 + _43 / view_z   ->   near at z_ndc = 0, far at z_ndc = 1
//     far = -_43 / (_33 - 1)
//
// With s = o = 0.5 and the logged runtime values (Near = 2.997619, Far = 1000, ProjZZ = -1.006013,
// ProjZW = -6.013264) that gives _33 = 1.003007, _43 = -3.006632, far = 999.9. Correct.
//
// With the city's s = 0.495, o = 0.495 (inside the portal pass) it gives _33 = 0.99297, and
// _33 < 1 means z_ndc *never reaches 1* - it asymptotes to 0.993 at infinity. Solving for the far
// plane yields a NEGATIVE distance. The frustum is not merely wrong, it does not exist. Anything
// that decomposes the projection matrix back into near/far - which is exactly what RTX Remix's
// camera reconstruction does, having no other source for them - gets nonsense and rejects the
// camera outright: "[RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera",
// followed by a zeroed camera transform ("Attempted invert a non-invertible matrix", then
// "WorldToScreenMatrix not invertible!" from DLSS every frame for the rest of the session).
//
// It also moved *between passes of one frame*, because ShadowZBias is subtracted for the portal
// pass and restored for agiTexSorter's - so the frame's geometry arrived under two different
// projections, which is the "CameraManager: FOV of a camera changed between frames" warning and
// the spurious camera cuts. And 0.005 of DepthOffset is enough to flip _33 across 1.0 on its own,
// so the failure is not even stable: at s = 0.499 the far plane comes out as 1500 instead of 1000.
//
// This is why the showroom path traced and the race did not, and it is not something the earlier
// in-scene-RHW work could have reached: mmCullCity does not exist until a city loads, so the menus
// and the car selector ran at the default 0.5/0.5 and produced a valid frustum by accident.
//
// Nothing is lost by dropping the guard band. It buys a software rasteriser some headroom at the
// endpoints; against a 24-bit depth buffer it only throws away 1% of the range, and the CPU path's
// own clamp (mrkni.cpp, KniMinZ/KniMaxZ) already pins depth to [0, 1] regardless. ShadowZBias is
// untouched and still works - see the note in agiDX9Pipeline::BeginFrame().
//
// `rh_view` folds the Z flip that view_zflip normally applies into this matrix instead, so
// D3DTS_VIEW can stay a proper rigid transform. Writing the current path out:
//
//     v' = v_world . view_zflip = (x, y, -z_v, 1)          [view_zflip negates View's Z column]
//     w_clip = -z_v . _34        with _34 = +1
//     z_clip = -z_v . _33 + _43
//
// and the right-handed path, with v = v_world . View = (x, y, z_v, 1):
//
//     w_clip = z_v . (-1)  = -z_v                          [same]
//     z_clip = z_v . (-_33) + _43 = -z_v . _33 + _43       [same]
//
// x and y are untouched by the flip either way, so clip space comes out bit-identical and screen
// winding does not change. See PARAM_d3d9_rhview.
static D3DMATRIX BuildProjectionMatrix(const agiViewParameters& p, bool rh_view = false)
{
    // NOTE: ProjXZ/ProjYZ deliberately do NOT appear here. The actual mesh transform
    // (agiMeshSet::ARTS_TRANSFORM_DOT, meshrend.cpp) computes output.x/output.y purely from the
    // combined M matrix, with no ProjXZ/ProjYZ term anywhere - those two fields are only used by
    // DrawCard()/LineList() for an unrelated purpose (scaling a billboard/line's screen-space
    // width by its view-space Z). An earlier version of this function mistook that for a lens-shear
    // term and folded it into the projection matrix, which skewed every mesh (including city-instance
    // buildings and car wheels) whenever ProjXZ/ProjYZ happened to be non-zero (e.g. in action cam).
    D3DMATRIX result {};

    result._11 = p.ProjX;
    result._22 = p.ProjY;

    // agiMeshSet::FlipX mirrors the scene horizontally about the viewport centre. It is set by the
    // still-closed rear-view-mirror setup (game.asm ~180652, cleared again at ~180940), and the CPU
    // pretransform path honours it by negating agiMeshSet::HalfWidth in InitViewport() - which
    // negates NDC X while leaving OffsX, the viewport centre, alone. Nothing on this path applied
    // it, so mirror content drawn through MeshWorld came out un-mirrored while the CPU-path content
    // sharing the same rectangle (particles, sprites) stayed mirrored - the two disagreed about
    // left and right inside the same small box. Negating _11 is the exact projection-matrix
    // equivalent of negating HalfWidth. No winding compensation is needed because this path
    // rasterises with D3DCULL_NONE (see MeshWorld) - the CPU-side plane test already culled.
    if (agiMeshSet::FlipX)
        result._11 = -result._11;

    // agiViewParameters::ProjZZ is stored in the *negative-forward* (OpenGL) convention the
    // engine's own perspective setup produces - confirmed against the logged runtime values
    // (Near=2.997619, Far=1000 -> ProjZZ=-1.006013 == -(F+N)/(F-N), ProjZW=-6.013264 ==
    // -2FN/(F-N)). agiMeshSet's CPU path does not use that field directly: agiMeshSet::Init()
    // copies it in *negated* (game.asm, `fld [ProjZZ]` / `fchs` / `fstp [agiMeshSet::ProjZZ]`),
    // because ARTS_TRANSFORM_DOT's `output.w` comes from M's Z-row, which is likewise the
    // negation of Dot(World, View)'s Z-row - i.e. the CPU path works in positive-forward view
    // depth. ProjZW is copied across unnegated.
    //
    // MeshWorld() feeds D3D9 the same positive-forward convention (see view_zflip there), so the
    // matching coefficient here is -p.ProjZZ, not p.ProjZZ. Using the raw field made
    //     _33 = -1.006013 * 0.5 + 0.5 = -0.003007
    // which drives z_clip = _33 * view_z + _43 negative for *every* view_z > 0 (_43 is itself
    // negative), so every triangle submitted through this path failed D3D9's 0 <= z_clip <= w_clip
    // near-plane test and was clipped away entirely - geometry silently vanishing while the draw
    // call still reported success. With the sign corrected, z_clip reaches 0 exactly at view_z ==
    // Near (1.003007 * 2.997619 - 3.006632 ~= 0) and z_clip/w_clip reaches 1 at view_z == Far,
    // matching ToScreen()'s `z * inv_w * DepthScale + DepthOffset` term for term.
    //
    // The NDC-halving constants are literals, not agiMeshSet::DepthScale/DepthOffset - see the
    // frustum note above. -d3d9legacydepth puts the globals back for A/B testing.
    const f32 depth_scale = PARAM_d3d9_legacydepth.get_or(false) ? agiMeshSet::DepthScale : 0.5f;
    const f32 depth_offset = PARAM_d3d9_legacydepth.get_or(false) ? agiMeshSet::DepthOffset : 0.5f;

    result._33 = -p.ProjZZ * depth_scale + depth_offset;
    result._34 = 1.0f;

    result._43 = p.ProjZW * depth_scale;

    if (rh_view)
    {
        result._33 = -result._33;
        result._34 = -1.0f;
    }

    return result;
}

static constexpr i32 kMaxWorldLights = 8;
static constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;

// --- Light and material state mirror ------------------------------------------------------------
//
// MeshWorld() re-programs the lights and the material on EVERY world draw (see the two Setup calls
// near the end of it). The values almost never differ between draws: the static rig reads
// agiMeshLighterSun/Fill1/Fill2 and their colours, which mmCullCity::Cull sets once a frame, and the
// material is a constant. At 706-1007 world draws a frame that was ~12 device calls each - three
// D3DLIGHT9 structs of 104 bytes, five LightEnable calls for slots nobody is using, and a
// D3DMATERIAL9 - for around 9,600 calls a frame that change nothing.
//
// Under RTX Remix that is not merely driver overhead. The game is a 32-bit client talking to a
// 64-bit server across shared memory, so every one of those is a command serialised into a queue and
// read back out by another process.
//
// This mirrors what was last sent and drops the call when it would be a repeat. Byte comparison
// rather than a "has the rig changed" flag on purpose: a flag has to be invalidated by every writer
// and silently goes stale when a new one appears, whereas comparing the bytes about to be sent is
// correct by construction and cannot drift from what the device actually holds.
//
// The only thing that can put the device out of step with this is losing it, so the reset path
// clears the mirror - see agiDX9InvalidateLightCache() and its callers in BeginGfx/EndGfx.
struct agiDX9LightMirror
{
    D3DLIGHT9 Lights[kMaxWorldLights];
    bool LightKnown[kMaxWorldLights];
    bool Enabled[kMaxWorldLights];
    bool EnabledKnown[kMaxWorldLights];

    D3DMATERIAL9 Material;
    bool MaterialKnown;
};

static agiDX9LightMirror g_LightMirror {};

void agiDX9InvalidateLightCache()
{
    g_LightMirror = {};
    agiDX9InvalidateStateCache();
}

static void MirroredSetLight(IDirect3DDevice9* device, DWORD index, const D3DLIGHT9& light)
{
    if (index >= kMaxWorldLights)
        return;

    if (agiDX9StateCacheEnabled() && g_LightMirror.LightKnown[index] &&
        (std::memcmp(&g_LightMirror.Lights[index], &light, sizeof(light)) == 0))
        return;

    g_LightMirror.Lights[index] = light;
    g_LightMirror.LightKnown[index] = true;

    device->SetLight(index, &light);
}

static void MirroredLightEnable(IDirect3DDevice9* device, DWORD index, bool enable)
{
    if (index >= kMaxWorldLights)
        return;

    if (agiDX9StateCacheEnabled() && g_LightMirror.EnabledKnown[index] && (g_LightMirror.Enabled[index] == enable))
        return;

    g_LightMirror.Enabled[index] = enable;
    g_LightMirror.EnabledKnown[index] = true;

    device->LightEnable(index, enable ? TRUE : FALSE);
}

static void MirroredSetMaterial(IDirect3DDevice9* device, const D3DMATERIAL9& material)
{
    if (agiDX9StateCacheEnabled() && g_LightMirror.MaterialKnown &&
        (std::memcmp(&g_LightMirror.Material, &material, sizeof(material)) == 0))
        return;

    g_LightMirror.Material = material;
    g_LightMirror.MaterialKnown = true;

    device->SetMaterial(&material);
}

static void SetupD3D9Lights(IDirect3DDevice9* device)
{
    i32 enabled = 0;

    // agiLighter::ACTIVELIGHTS is a per-object scratch cache written only by the still-closed
    // assembly CPU-lighting machinery (agiLighter::LightVertex and friends) - since this
    // world-space path bypasses CPU lighting entirely, that cache is never refreshed for us and
    // can hold dangling pointers left over from lights destroyed since the last CPU-lit draw
    // (this crashed with an access violation here after a race reset tore down its lights).
    // agiLighter::LIGHTS[0, Current) is instead maintained directly by real, reimplemented C++
    // (DeclareLight(), called from each agiLight's constructor), so it stays valid for the
    // lifetime of the lights it references - use that instead.
    // Lighting quality caps the dynamic rig too, and here it maps onto something real rather than
    // onto a shape: these are genuine per-light costs on the fixed-function unit, so fewer active
    // D3DLIGHT9 slots is exactly what a lower setting should buy. The list is already ordered by
    // the engine's own declaration order, so the lights that survive are stable frame to frame
    // rather than flickering as the cap bites.
    const i32 light_budget = (agiRQ.LightQuality >= AGI_QUALITY_VERY_HIGH) ? kMaxWorldLights
        : (agiRQ.LightQuality >= AGI_QUALITY_HIGH)                         ? 6
        : (agiRQ.LightQuality >= AGI_QUALITY_MEDIUM)                       ? 3
                                                                           : 0;

    for (i32 i = 0; i < agiLighter::Current && enabled < light_budget; ++i)
    {
        agiLight* light = agiLighter::LIGHTS[i];

        if (!light)
            continue;

        const agiLightParameters& params = light->Params;

        bool positional = params.Position.w != 0.0f;
        bool spot = positional && params.SpotAngle < 180.0f;

        D3DLIGHT9 d3dlight {};
        d3dlight.Type = !positional ? D3DLIGHT_DIRECTIONAL : (spot ? D3DLIGHT_SPOT : D3DLIGHT_POINT);

        d3dlight.Diffuse = {params.Diffuse.x, params.Diffuse.y, params.Diffuse.z, params.Alpha};
        d3dlight.Ambient = {params.Ambient.x, params.Ambient.y, params.Ambient.z, 1.0f};
        d3dlight.Specular = {params.Specular.x, params.Specular.y, params.Specular.z, 1.0f};

        d3dlight.Position = {params.Position.x, params.Position.y, params.Position.z};
        d3dlight.Direction = {params.Direction.x, params.Direction.y, params.Direction.z};

        d3dlight.Range = 1.0e8f; // Effectively unbounded (D3DLIGHT_RANGE_MAX isn't in every SDK's headers)
        d3dlight.Falloff = 1.0f;
        d3dlight.Attenuation0 = params.ConstantAtten;
        d3dlight.Attenuation1 = params.LinearAtten;
        d3dlight.Attenuation2 = params.QuadraticAtten;

        if (spot)
        {
            // Best-effort mapping - the original (still-ARTS_IMPORT) lighting code may use a
            // different spot falloff curve. Using D3D9's own spot cone here, per the plan.
            d3dlight.Phi = std::clamp(params.SpotAngle * kDegToRad, 0.01f, 3.13f);
            d3dlight.Theta = d3dlight.Phi * 0.5f;
            d3dlight.Falloff = (params.SpotExp > 0.0f) ? params.SpotExp : 1.0f;
        }

        // The D3D9 light slot must be `enabled`, not `i`. agiLighter::LIGHTS is a 16-entry array
        // that is allowed to contain holes (RemoveLight() clears a slot without compacting it out),
        // and agiLighter::Current is its high-water mark, not a dense count. Indexing D3D9 by `i`
        // broke both ways. A hole before a live light left `enabled < i`, so the disable loop below
        // - which starts at `enabled` - immediately switched off a slot this loop had just filled,
        // and that light silently contributed nothing. And with more than 8 live lights `i` ran on
        // past kMaxWorldLights while `enabled` stopped at it, so slots 8..15 got enabled and were
        // never disabled again: they leaked into every later draw and exceeded the
        // D3DCAPS9::MaxActiveLights most drivers report.
        MirroredSetLight(device, static_cast<DWORD>(enabled), d3dlight);
        MirroredLightEnable(device, static_cast<DWORD>(enabled), true);

        ++enabled;
    }

    for (i32 i = enabled; i < kMaxWorldLights; ++i)
        MirroredLightEnable(device, static_cast<DWORD>(i), false);
}

// The city's static rig has no specular term at all. agiMeshLighterTriple (agiworld/meshlight.cpp,
// mmxTriple) computes intensity = key + fill1 + fill2 + ambient and multiplies it into the vertex
// colour - three clamped N.L lobes and nothing else. There is no half-vector, no exponent, no
// specular colour anywhere in it.
//
// Adding one here gave every building facade and every road surface a moving highlight the original
// never had. It reads as wrong for three compounding reasons: the surfaces are flat and enormous,
// so the highlight sweeps across a whole wall at once; the lighting is per-vertex, so the highlight
// is interpolated across metre-scale triangles rather than resolved; and agiWorldVtx::normal comes
// from UnpackNormal[], a 198-entry quantised direction table, so adjacent facets that should share
// a normal often differ by several degrees and the specular power amplifies exactly that error into
// visible facet-by-facet banding. That is the reported "buildings and roads gleam and shine at
// weird angles".
//
// Pathway A is the parity path, so this is off by default. It stays available because it is a
// reasonable thing to want on a modern display - but per-pixel specular over real normals belongs
// to a programmable path, not to a per-vertex fixed-function approximation.
static mem::cmd_param PARAM_d3d9_specular {"d3d9specular", "Add a specular term to the static city lighting rig"};

bool agiDX9WantsStaticSpecular()
{
    return PARAM_d3d9_specular.get_or(false);
}

// Builds the fixed sun/fill1/fill2 + ambient rig agiMeshLighterTriple/Quarter compute on the CPU
// for StaticLighter/DynamicLighter-lit content (city buildings/terrain - see meshrend.cpp's
// DrawLit) as three D3D9 directional lights, so hardware-transformed draws of that content are
// lit the same way instead of starved by the real-time dynamic light list (SetupD3D9Lights()/
// agiLighter::LIGHTS[]) built for cars/wheels - two genuinely different, unrelated light sources
// in the original engine (confirmed via disassembly: routing buildings through the dynamic list
// produced near-black output at night). Mirrors agiMeshLighterTriple's math exactly
// (agiworld/meshlight.cpp): per light, intensity = max(0, N . direction-to-light), modulated by
// the light's color, summed with a flat ambient term, then multiplied into the vertex color -
// D3D9's own directional lights plus a D3DMCS_COLOR1 diffuse source compute exactly this once the
// directions/colors/ambient below are set to match.
static void SetupD3D9StaticLights(IDirect3DDevice9* device, bool sun_per_pixel)
{
    auto set_directional = [device](DWORD index, const Vector3& direction_to_light, const Vector3& color) {
        D3DLIGHT9 light {};
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse = {color.x, color.y, color.z, 1.0f};

        // Matches SetupD3D9StaticMaterial() - the original rig has no specular lobe, so by default
        // neither do these lights. See the note there.
        if (agiDX9WantsStaticSpecular())
            light.Specular = {color.x * 0.55f, color.y * 0.55f, color.z * 0.55f, 1.0f};
        // agiMeshLighterTriple's `direction` points toward the light source (meshlight.cpp); D3D9
        // directional lights instead specify the direction the light travels - the negation.
        light.Direction = {-direction_to_light.x, -direction_to_light.y, -direction_to_light.z};
        light.Range = 1.0e8f;

        MirroredSetLight(device, index, light);
        MirroredLightEnable(device, index, true);
    };

    // With -ffperpixel the sun is evaluated per fragment in its own additive pass, so it must NOT
    // also be summed here - leaving it on would light the surface twice, once smeared across the
    // triangle and once correctly. The two fills stay on the lighting unit; see dx9ffshade.h for why
    // only the sun is worth the extra passes.
    if (sun_per_pixel)
        MirroredLightEnable(device, 0, false);
    else
        set_directional(0, agiMeshLighterSun, agiMeshLighterSunColor);

    // The graphics menu's Lighting option, applied where it can actually be seen.
    //
    // agiRQ.LightQuality used to have no effect at all on this backend, and the reason is worth
    // stating because it is not obvious from either side. The setting's only job in the original is
    // to pick a CPU lighter function - fix_lighting (mmcity/cullcity.cpp) maps it onto
    // agiMeshLighterQuarter or agiMeshLighterTriple - and this path does not call lighter functions.
    // It reads the pointer solely to decide static rig versus dynamic rig (IsStaticCityLighter), and
    // BOTH candidates answer "static", so MEDIUM, HIGH and VERY HIGH all arrived here as the same
    // three-light rig and the menu item did nothing but relabel itself. LOW was the sole exception,
    // and only because it sets the lighter to null, which routes the draw elsewhere entirely.
    //
    // The fill lights are what the setting now controls, which is the closest honest analogue of
    // what it means on the CPU: agiMeshLighterQuarter is the cheaper rig and Triple the full one, so
    // dropping fills as quality drops reproduces that shape rather than inventing a new meaning.
    // The sun always survives - a scene with no key light is not a lower quality setting, it is a
    // different picture.
    const bool fill1 = agiRQ.LightQuality >= AGI_QUALITY_MEDIUM;
    const bool fill2 = agiRQ.LightQuality >= AGI_QUALITY_VERY_HIGH;

    if (fill1)
        set_directional(1, agiMeshLighterFill1, agiMeshLighterFill1Color);
    else
        MirroredLightEnable(device, 1, false);

    if (fill2)
        set_directional(2, agiMeshLighterFill2, agiMeshLighterFill2Color);
    else
        MirroredLightEnable(device, 2, false);

    for (i32 i = 3; i < kMaxWorldLights; ++i)
        MirroredLightEnable(device, static_cast<DWORD>(i), false);
}

// agiMeshLighterTriple has no material concept - it multiplies the summed sun/fill/ambient
// intensity directly into the vertex color, i.e. an implicit Ambient/Diffuse material of exactly
// (1,1,1,1). Deliberately ignores agiCurState.GetMtl() - that belongs to the dynamic
// agiLighter::LIGHTS[] path and its own materials, not this rig.
static void SetupD3D9StaticMaterial(IDirect3DDevice9* device)
{
    D3DMATERIAL9 d3dmtl {};
    d3dmtl.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
    d3dmtl.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};

    if (agiDX9WantsStaticSpecular())
    {
        d3dmtl.Specular = {0.30f, 0.30f, 0.30f, 1.0f};
        d3dmtl.Power = 20.0f;
    }

    MirroredSetMaterial(device, d3dmtl);
}

static void SetupD3D9Material(IDirect3DDevice9* device)
{
    D3DMATERIAL9 d3dmtl {};

    if (agiMtlDef* mtl = agiCurState.GetMtl())
    {
        const agiMtlParameters& params = mtl->Mtl;

        d3dmtl.Diffuse = {params.Diffuse.x, params.Diffuse.y, params.Diffuse.z, params.Diffuse.w};
        d3dmtl.Ambient = {params.Ambient.x, params.Ambient.y, params.Ambient.z, params.Ambient.w};
        d3dmtl.Specular = {params.Specular.x, params.Specular.y, params.Specular.z, params.Specular.w};
        d3dmtl.Emissive = {params.Emmisive.x, params.Emmisive.y, params.Emmisive.z, params.Emmisive.w};
        d3dmtl.Power = params.Power;
    }
    else
    {
        d3dmtl.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
        d3dmtl.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};
        d3dmtl.Specular = {0.35f, 0.35f, 0.35f, 1.0f};
        d3dmtl.Power = 24.0f;
    }

    MirroredSetMaterial(device, d3dmtl);
}

// Restores every piece of device state MeshWorld() programs, for both pathways. Shared so the
// fixed-function and programmable branches cannot drift apart on cleanup - which is the failure
// mode this whole file has been bitten by repeatedly (see the agiLastState note below).
void agiDX9Rasterizer::LeaveWorldState()
{
    if (!world_state_active_)
        return;

    world_state_active_ = false;

    RestoreStateAfterWorldDraw(world_remap_vertex_fog_);
}

void agiDX9Rasterizer::RestoreStateAfterWorldDraw(bool remap_vertex_fog)
{
    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    // Pretransformed (agiScreenVtx) draws ignore D3DRS_LIGHTING entirely, but reset it anyway
    // for clarity - and so BeginGfx()'s initial state assumption keeps holding.
    WorldSetRenderState(device, D3DRS_LIGHTING, FALSE);
    WorldSetRenderState(device, D3DRS_SPECULARENABLE, FALSE);

    // Pretransformed draws are not lit either, so this has no effect on them - but leaving it set
    // would still be a lie about the device's state, and it costs the driver work on every
    // subsequent vertex. Put it back the way BeginGfx() left it.
    WorldSetRenderState(device, D3DRS_NORMALIZENORMALS, FALSE);

    // Vertex blending off. Unconditional, and not optional: this is the one piece of state a
    // skinned draw leaves behind that would silently corrupt every following draw rather than merely
    // mis-shade it. With D3DVBF_0WEIGHTS still set, D3D9 reads a matrix index out of a vertex layout
    // that has no beta field at all and transforms by whatever palette slot that lands on - so an
    // XYZ or XYZRHW draw following a pedestrian would be placed by a leftover thigh bone. Restoring
    // it here rather than at the end of the skinned branch keeps it on the same shared path as every
    // other piece of state MeshWorld programs, which is the drift this function exists to prevent.
    WorldSetRenderState(device, D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    WorldSetRenderState(device, D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);

    // Reset the depth bias so it doesn't leak into pretransformed (agiScreenVtx) draws, which
    // have no need for it and aren't tracked by FlushState()'s dirty-checking anyway.
    constexpr f32 kNoDepthBias = 0.0f;
    WorldSetRenderState(device, D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&kNoDepthBias));

    // Undo the vertex-fog remap above. FlushState() caches fog state in agiLastState and only
    // re-issues it on a change, so table fog left switched on here would silently apply itself to
    // every following CPU-path draw - whose vertices carry the fog factor in specular alpha and
    // would then be fogged twice.
    if (remap_vertex_fog)
        WorldSetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);

    // Fog enable is restored unconditionally, because two separate things above may have switched
    // it off for this draw - the vertex-fog remap and the additive-glow suppression - and the
    // programmable path switches it off too (dx9shader.cpp). One truthful restore covers all three.
    WorldSetRenderState(device, D3DRS_FOGENABLE, (agiLastState.FogMode != agiFogMode::None) ? TRUE : FALSE);

    // Put the device back exactly as agiLastState describes it.
    //
    // Earlier revisions instead poisoned agiLastState (CullMode/Texture/TexEnv/AlphaEnable set to
    // impossible values) to force the next FlushState() to re-apply everything. That does not
    // work, and it is why alpha-keyed content went black across the city: FlushState() begins with
    //     if (!agiCurState.IsTouched()) return;
    // and `touched_` is only set when a *value actually changes* (agi/rsys.h, agiRendState::Set).
    // A following sprite draw that happens to want the same state the engine already had never
    // touches agiCurState, FlushState() returns immediately, and the poisoned agiLastState is
    // never even looked at - so the device silently kept this function's lighting-oriented setup.
    // Every glow and flare (taillights, traffic lights, street lamps, coronas) is drawn that way,
    // with additive/alpha blending the engine had set up long before, so they rasterised their
    // black texture background opaque and appeared as black boxes with the glow inside.
    //
    // Restoring the device to match agiLastState keeps that cache truthful, so it stays correct
    // whether or not FlushState() runs next.
    WorldSetRenderState(device, D3DRS_ALPHABLENDENABLE, agiLastState.AlphaEnable ? TRUE : FALSE);
    WorldSetRenderState(device, D3DRS_ALPHATESTENABLE, agiLastState.AlphaEnable ? TRUE : FALSE);

    if (agiLastState.AlphaEnable)
    {
        WorldSetRenderState(device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        WorldSetRenderState(device, D3DRS_ALPHAREF, agiLastState.AlphaRef);
    }

    WorldSetRenderState(device, D3DRS_CULLMODE, agiNoCullEnabled() ? D3DCULL_NONE : ToD3DCull(agiLastState.CullMode));

    // Depth writes, restored for the same reason as everything else here: the additive-glow branch
    // above may have switched them off, and FlushState() only re-issues D3DRS_ZWRITEENABLE when
    // agiCurState's value changes, so leaving it off would silently disable depth writes for every
    // following draw that happened not to toggle it.
    WorldSetRenderState(device, D3DRS_ZWRITEENABLE, agiLastState.ZWrite ? TRUE : FALSE);

    // Blend mode is programmed above now, so it has to be put back too - agiLastState.BlendSet is
    // what FlushState() believes the device is in, and it only re-issues on a *change*, so leaving
    // this path's blend mode behind would silently apply it to every following CPU-path draw that
    // happens not to change blend set.
    //
    // Guarded because agiRendStateStruct::Reset() (agi/rsys.cpp) poisons the whole struct with
    // 0xFF, which is not a valid agiBlendSet - restoring blind would hit ApplyBlendSet()'s
    // "bad blend mode" Quitf if this path ever ran before the first full FlushState().
    if (agiLastState.BlendSet <= agiBlendSet::One_One)
        ApplyBlendSet(device, agiLastState.BlendSet);

    agiDX9TexDef* restore_tex = static_cast<agiDX9TexDef*>(agiLastState.Texture);
    IDirect3DTexture9* restore_handle = restore_tex ? restore_tex->GetHandle() : nullptr;

    WorldSetTexture(device, 0, restore_handle);
    current_texture_ = restore_handle;

    // Sampler filter *and address* state is global to stage 0, and agiDX9TexDef::SetFilters()
    // programs both from the texture it is called for (see the D3DTADDRESS note there). The
    // ApplyTexFilters() call above therefore left this draw's texture's wrap/clamp modes on the
    // device. Rebinding restore_handle does not undo that: FlushState() only calls
    // ApplyTexFilters() when it sees the bound texture *change*, and from its point of view nothing
    // changed - agiLastState.Texture still names the same texture it did before this call. So a
    // CLAMP-mode world texture (road and terrain sheets are clamped) silently left stage 0 in
    // CLAMP, and the next CPU-path draw of a WRAP texture tiled nothing, smearing its edge texels
    // across the surface. Re-apply the restored texture's own filters so the device matches what
    // the cache claims.
    if (restore_handle)
        ApplyTexFilters(restore_tex, agiLastState.TexFilter);

    ApplyTexEnv(device, tex_env_);

    // Per the D3D9 spec, XYZRHW (pretransformed) draws should completely ignore
    // D3DTS_WORLD/VIEW/PROJECTION - but some drivers (notably Intel's fixed-function-pipeline
    // implementations) have known quirks where leftover transform state affects supposedly
    // transform-immune draws (e.g. via internal clipping or fog referencing view-space Z). Reset
    // all three to identity so every subsequent pretransformed (agiScreenVtx) draw this frame is
    // guaranteed a clean slate, regardless of what this hardware-transform draw just set them to.
    D3DMATRIX identity {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;

    WorldSetTransform(device, D3DTS_WORLD, identity);
    WorldSetTransform(device, D3DTS_VIEW, identity);

    // PROJECTION is deliberately NOT reset, for RTX Remix's benefit.
    //
    // Remix classifies a fixed-function draw as UI when the current projection looks orthographic
    // (_44 == 1.0f, and identity qualifies) and D3DRS_ZWRITEENABLE is off. The first "UI" draw of a
    // frame latches RTX injection - every draw after it that frame is rasterized only, never path
    // traced, and the game still looks normal because rtx.skipDrawCallsPostRTXInjection defaults
    // false. This engine draws its screen-space effects mid-scene - blob shadows, glow cards, smoke
    // - all RHW with zwrite off, so an identity projection left behind here means the frame's first
    // blob shadow quietly ends path tracing for everything after it.
    //
    // The census measures exactly how much that costs: the showroom issues ZERO in-scene RHW draws
    // and path traces correctly, while gameplay issues 12-27 of them EVERY frame. That is the whole
    // "Remix works in the showroom but not in the race" report.
    //
    // This used to be conditional on agiDX9RemixBridgeActive(), which could not work: the test was
    // for "remix" in the requested DLL name, but Remix's normal install is a drop-in called
    // `d3d9.dll` and chaining proxies are called `d3d9.dll` too. The guard was dead code in every
    // real configuration, which is why the symptom survived the fix that was supposed to cure it.
    // Nothing is gained by knowing whether Remix is loaded, so the condition is gone: leaving the
    // last perspective projection in place is free either way, because RHW draws ignore all three
    // transforms per the D3D9 spec. The genuine HUD still composites, because Present() injects RTX
    // at end of frame regardless.
    //
    // -d3d9identityproj restores the old unconditional reset, for the driver quirk described above
    // (leftover transform state reaching supposedly transform-immune draws) if it ever shows up.
    if (PARAM_d3d9_identityproj.get_or(false))
        WorldSetTransform(device, D3DTS_PROJECTION, identity);
}

// Harvests a world-space glow mesh as a light source. See agiworld/glowlight.h.
//
// The billboard path (agiMeshSet::DrawCard) misses everything vehicle-related: mmCarModel::DrawGlow
// and aiVehicleInstance::DrawGlow both submit a glow MESH, so head/tail/brake/reverse lights never
// reach it. This covers those, and any other AlphaGlow geometry in the city.
//
// Position and extent come from the submitted geometry rather than from the instance origin,
// because a tail light sits well off a car's centre and the whole point is that the light lands
// where the lamp is.
//
// [[maybe_unused]] because its caller is gone with Pathway B and this is kept deliberately: a
// static function with no references is C4505 at /W4, which /WX makes fatal. The attribute says
// "unreferenced on purpose" rather than silencing the warning globally.
[[maybe_unused]] static void HarvestWorldGlow(
    agiDX9TexDef* texture, agiWorldVtx* vertices, u16* indices, i32 index_count, const Matrix34& world)
{
    if (!texture || (index_count <= 0))
        return;

    // One light PER FLARE, not one per glow mesh.
    //
    // A vehicle's glow mesh holds every lamp on one end of the car in a single submission - both
    // headlights, or both tail lights - so averaging the whole thing into a single centroid put one
    // light on the car's centre axis, between the actual lamps. That is exactly the reported "cars
    // have only one light right in the car's center axis; there should be two, right where their
    // alpha cards are".
    //
    // Each flare is a separate quad, so clustering triangles by proximity recovers them without
    // needing to know anything about how a particular car was modelled - it works for two
    // headlights, for a single centre brake light, or for the four-lamp arrangements some vehicles
    // use. The threshold is derived from the mesh's own extent rather than fixed in world units, so
    // it scales from a motorbike to a bus.
    constexpr i32 kMaxClusters = 4;

    struct Cluster
    {
        Vector3 Sum;
        Vector3 Min;
        Vector3 Max;
        f32 AccumR, AccumG, AccumB, AccumA;
        f32 AccumU, AccumV;
        i32 Count;
    };

    Cluster clusters[kMaxClusters] {};
    i32 cluster_count = 0;

    // Overall extent first, to size the clustering threshold.
    Vector3 bound_min = vertices[indices[0]].pos;
    Vector3 bound_max = bound_min;

    for (i32 i = 1; i < index_count; ++i)
    {
        const Vector3& p = vertices[indices[i]].pos;

        bound_min = {std::min(bound_min.x, p.x), std::min(bound_min.y, p.y), std::min(bound_min.z, p.z)};
        bound_max = {std::max(bound_max.x, p.x), std::max(bound_max.y, p.y), std::max(bound_max.z, p.z)};
    }

    const Vector3 extent = bound_max - bound_min;
    const f32 threshold = std::max(std::max({extent.x, extent.y, extent.z}) * 0.25f, 0.05f);
    const f32 threshold_sq = threshold * threshold;

    for (i32 tri = 0; (tri + 2) < index_count; tri += 3)
    {
        Vector3 centre =
            (vertices[indices[tri]].pos + vertices[indices[tri + 1]].pos + vertices[indices[tri + 2]].pos) / 3.0f;

        i32 slot = -1;

        for (i32 c = 0; c < cluster_count; ++c)
        {
            const Vector3 mean = clusters[c].Sum / static_cast<f32>(clusters[c].Count);

            if ((mean - centre).Mag2() <= threshold_sq)
            {
                slot = c;
                break;
            }
        }

        if (slot < 0)
        {
            if (cluster_count >= kMaxClusters)
                continue;

            slot = cluster_count++;
            clusters[slot] = {};
            clusters[slot].Min = centre;
            clusters[slot].Max = centre;
        }

        Cluster& cluster = clusters[slot];

        for (i32 k = 0; k < 3; ++k)
        {
            const agiWorldVtx& vtx = vertices[indices[tri + k]];

            cluster.Min = {std::min(cluster.Min.x, vtx.pos.x), std::min(cluster.Min.y, vtx.pos.y),
                std::min(cluster.Min.z, vtx.pos.z)};
            cluster.Max = {std::max(cluster.Max.x, vtx.pos.x), std::max(cluster.Max.y, vtx.pos.y),
                std::max(cluster.Max.z, vtx.pos.z)};

            cluster.AccumR += static_cast<f32>((vtx.color >> 16) & 0xFF);
            cluster.AccumG += static_cast<f32>((vtx.color >> 8) & 0xFF);
            cluster.AccumB += static_cast<f32>(vtx.color & 0xFF);
            cluster.AccumA += static_cast<f32>((vtx.color >> 24) & 0xFF);

            cluster.AccumU += vtx.tu;
            cluster.AccumV += vtx.tv;
        }

        cluster.Sum += centre;
        ++cluster.Count;
    }

    for (i32 c = 0; c < cluster_count; ++c)
    {
        const Cluster& cluster = clusters[c];

        const f32 inv_verts = 1.0f / static_cast<f32>(cluster.Count * 3);

        Vector3 local_centre = (cluster.Min + cluster.Max) * 0.5f;

        Vector3 world_centre;
        world_centre.Dot(local_centre, world);

        const Vector3 half = (cluster.Max - cluster.Min) * 0.5f;
        const f32 flare_size = std::max(std::max({half.x, half.y, half.z}), 0.05f);

        // The vertex colour is only the TINT. The hue comes from the glow texture itself, sampled at
        // the UVs this cluster reads - see agiDX9TexDef::SampleGlowColor.
        const f32 intensity = std::max((cluster.AccumA * inv_verts) / 255.0f, 1.0f / 255.0f);

        Vector3 tint {
            (cluster.AccumR * inv_verts / 255.0f) * intensity,
            (cluster.AccumG * inv_verts / 255.0f) * intensity,
            (cluster.AccumB * inv_verts / 255.0f) * intensity,
        };

        // agiGlowLightReach() is shared with the billboard route (agiAddGlowLight). The two used to
        // floor the reach differently - 14 here against 24 there - and since emitted intensity goes
        // with the square of reach, that alone made the same fixture ~3x brighter as a card than as
        // a mesh.
        agiAddGlowLightRGB(world_centre, tint, agiGlowLightReach(flare_size), texture, cluster.AccumU * inv_verts,
            cluster.AccumV * inv_verts);
    }
}

// Size of the fixed-function matrix palette this device can skin with. Never 0: one means no
// palette, and a skinned model is then submitted one bone at a time against a plain world matrix,
// which needs nothing of the device at all.
//
// Deliberately NOT gated on the programmable path. A skinned draw takes the fixed-function branch of
// MeshWorld() either way (see the note there), so a bound world shader does not cost a pedestrian
// its palette - it only means the pedestrian is one of the things the shader does not draw.
//
// Capped at 256 because that is the width of the index the vertex carries - one byte.
u32 agiDX9Rasterizer::MaxNativeSkinBones() const
{
    if (PARAM_noskin.get_or(false))
        return 1;

    agiDX9Context* context = Pipe()->Context();

    if (!context || (context->GetMaxVertexBlendMatrices() < 1))
        return 1;

    return std::max<u32>(std::min<u32>(context->GetSkinPaletteSize(), 256), 1);
}

// Splices the bone index into each vertex, in the layout D3D9 wants it. Everything else is a
// straight copy - the positions are the mesh's stored bind-pose values and stay untouched, which is
// the whole point of skinning on the GPU.
static void BuildSkinVertices(
    DX9SkinVtx* ARTS_RESTRICT output, const agiWorldVtx* ARTS_RESTRICT input, i32 count, const u8* ARTS_RESTRICT slots)
{
    for (i32 i = 0; i < count; ++i)
    {
        output[i].pos = input[i].pos;
        output[i].indices = slots[i];
        output[i].normal = input[i].normal;
        output[i].color = input[i].color;
        output[i].tu = input[i].tu;
        output[i].tv = input[i].tv;
    }
}

bool agiDX9Rasterizer::MeshWorld(agiWorldVtx* vertices, i32 vertex_count, u16* indices, i32 index_count,
    const Matrix34& world, const Matrix34& view, const agiViewParameters& proj_params, bool static_lighting,
    const agiNativeMaterialFx* fx, bool hardware_lighting, const agiNativeSkinPalette* skin)
{
    if (!IsAppActive() || (vertex_count == 0) || (index_count == 0))
        return true;

    FlushState();

    ARTS_UTIMED(agiRasterization);
    ++STATS.GeomCalls;

    ++agiDX9Census.WorldCalls;
    agiDX9Census.WorldTris += index_count / 3;

    if (!hardware_lighting)
        agiDX9Census.WorldUnlitTris += index_count / 3;
    else if (static_lighting)
        agiDX9Census.WorldStaticLitTris += index_count / 3;

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    // Depth bias, now off by default.
    //
    // This existed to win one specific fight: wheel/tyre geometry sitting millimetres above the
    // road was losing the depth test against the road, because the road was still drawn by the CPU
    // pretransform path. The two paths compute the same depth by different routes - the CPU as a
    // scalar chain (view_z * ProjZZ + ProjZW) * inv_w * DepthScale + DepthOffset, this path via the
    // GPU's perspective divide of a matrix-transformed clip Z - and they agree mathematically but
    // not bit-for-bit, which is enough to flip the winner for two coplanar surfaces.
    //
    // That premise no longer holds. DrawLitEnv() routes the road through this same path now, so
    // wheels and road are both hardware-transformed and compute their depth identically; there is
    // no cross-path comparison left to lose. What remains is a constant pull toward the camera
    // applied to *all* world geometry, biasing it against everything still on the CPU path - blob
    // shadows, tyre smoke, glows and decals, which sit close to surfaces by design and are exactly
    // the content a global bias makes fight.
    //
    // Kept as a parameter rather than deleted because the census still reports a nonzero in-scene
    // screen-triangle count, so some 3D content has not moved over yet.
    const f32 depth_bias = PARAM_d3d9_depthbias.get_or(0.0f);
    WorldSetRenderState(device, D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&depth_bias));

    // FlushState()'s texture/texture-stage bookkeeping (current_texture_/tex_env_) only updates
    // when agiCurState::DrawMode == agiDrawTextured - a piece of global state set deep inside the
    // still-closed CPU FirstPass()/material-bind code, which this native-transform path never
    // establishes since it bypasses the CPU pretransform pipeline entirely. Relying on that
    // leftover state here silently no-op'd every draw whenever the last thing drawn earlier in
    // the frame happened to leave DrawMode/tex_env_ in the "wrong" shape (confirmed via testing:
    // vehicle bodies - player and AI traffic - rendered completely invisible while still
    // reporting success, because current_texture_ had been zeroed by unrelated leftover state).
    // Bind the texture and its stage ops directly from agiCurState here instead, independent of
    // the pretransform path's DrawMode-gated bookkeeping.
    agiDX9TexDef* native_tex = static_cast<agiDX9TexDef*>(agiCurState.GetTexture());
    IDirect3DTexture9* native_handle = native_tex ? native_tex->GetHandle() : nullptr;

    WorldSetTexture(device, 0, native_handle);

    if (native_handle)
    {
        // FlushState() only applies these when it sees the bound texture *change*, and it is
        // gated on DrawMode == agiDrawTextured, which this path never establishes - so without
        // this the native path inherited whatever filters/addressing the last CPU-path draw left,
        // including the wrong wrap/clamp mode for this texture.
        ApplyTexFilters(native_tex, agiCurState.GetTexFilter());

        WorldSetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        WorldSetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        WorldSetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        WorldSetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        WorldSetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        WorldSetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    }
    else
    {
        WorldSetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        WorldSetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        WorldSetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        WorldSetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }

    current_texture_ = native_handle;

    // Alpha state has to be derived here too, for the same reason the texture bind above does.
    // FlushState() computes it as
    //     alpha_enable = agiCurState.GetAlphaEnable()
    //                  | texture->Tex.HasAlpha() | texture->Tex.UseChromakey()
    // but its `texture` is only non-null when agiCurState::DrawMode == agiDrawTextured - a mode
    // this path never establishes, since it bypasses the CPU pretransform pipeline that sets it.
    // So FlushState() saw no texture, never ORed in the per-texture terms, and left
    // ALPHABLENDENABLE/ALPHATESTENABLE off for exactly the textures that need them: every
    // alpha-keyed texture (light flares, coronas, tree and foliage billboards, fences, railings)
    // rasterised its transparent texels as opaque, which is the field of solid black quads that
    // showed up across the city once this path started carrying most of the scene.
    bool alpha_enable = agiCurState.GetAlphaEnable();

    if (native_tex)
        alpha_enable |= native_tex->Tex.HasAlpha() || native_tex->Tex.UseChromakey();

    // AlphaGlow textures (light flares, coronas, headlight/taillight glows, the fake headlight
    // cones) are the case the two terms above deliberately do NOT cover. agiTexProp::AlphaGlow
    // content is meant to be composited *additively* - the texture's black background contributes
    // nothing under One_One - so both getmesh.cpp (Tex.Flags &= ~Alpha when the renderer reports
    // AdditiveBlending, which agisdl/sdlsetup.cpp always does) and agiDX9TexDef::BeginGfx() strip
    // the Alpha flag from them at load. HasAlpha()/UseChromakey() are therefore both false, so
    // without this term alpha_enable stays false, ALPHABLENDENABLE goes off, and the glow's black
    // background rasterises opaque - a solid black box with the glow inside it.
    //
    // On the CPU path this never arises, because the draw is issued by agiTexSorter::DoTexture()
    // (closed assembly, game.asm), which binds blend state per texture from these same flags.
    // This path bypasses the sorter entirely, so it has to do that binding itself - the same class
    // of omission already fixed above for the texture bind and below for the blend mode.
    bool additive_glow = native_tex && (native_tex->Tex.Props & agiTexProp::AlphaGlow);

    u8 alpha_ref = agiCurState.GetAlphaRef();

    // Alpha *blending* and alpha *testing* are deliberately decoupled for the glow case. Blending
    // is what makes the black background vanish under One_One and must be on. Testing must NOT be
    // turned on for it: the glow was rebuilt as an X8R8G8B8 texture when its Alpha flag was
    // stripped, so its sampled alpha is a constant 1.0 and the stage modulates that by the vertex
    // diffuse alpha - which for a fading glow is small. A GREATER/AlphaRef test against that would
    // discard exactly the faint pixels the additive blend exists to draw.
    WorldSetRenderState(device, D3DRS_ALPHABLENDENABLE, (alpha_enable || additive_glow) ? TRUE : FALSE);
    WorldSetRenderState(device, D3DRS_ALPHATESTENABLE, alpha_enable ? TRUE : FALSE);

    if (alpha_enable)
    {
        WorldSetRenderState(device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        WorldSetRenderState(device, D3DRS_ALPHAREF, alpha_ref);
    }

    // Additive glows must not write depth.
    //
    // A glow quad is a transparent overlay: its black background contributes nothing to colour under
    // One_One, but if it writes Z it still stakes a claim on those pixels. Anything drawn afterwards
    // and behind it - snow, rain, tyre smoke, other glows - is then depth-rejected across the whole
    // quad, so the particle field develops a rectangular hole exactly the size of the sprite. That
    // is the reported "brake lights and headlights cull the particles near them, so the sharp
    // corners become visible": the corners being seen are the glow quad's own bounds, punched out
    // of the particles behind it.
    //
    // The CPU path never hits this because its draws go through agiTexSorter, which binds depth
    // state per texture from the same flags. This path bypasses the sorter, so it has to do it here
    // - the same class of omission already fixed for the texture bind, the blend mode and fog.
    // Programmed unconditionally now, and mirrored, rather than only being switched off for a glow
    // and put back by the restore. The restore no longer runs between two world draws (see
    // LeaveWorldState), so "only touch it when this draw is a glow" would leave the glow's setting
    // on every following world draw until something else happened to change it.
    WorldSetRenderState(device, D3DRS_ZWRITEENABLE, (!additive_glow && agiLastState.ZWrite) ? TRUE : FALSE);

    // Glow harvesting is unwired along with the rest of Pathway B (see agiDX9Pipeline::BeginGfx).
    // This was the mesh route - vehicle head, tail, brake and reverse lights, which never reach
    // agiMeshSet::DrawCard. HarvestWorldGlow() below is intact and simply has no caller.
    //
    // To re-wire:
    //     if (additive_glow && Pipe()->IsInScene())
    //         HarvestWorldGlow(native_tex, vertices, indices, index_count, world);
    //
    // IsInScene() is not incidental - it keeps menu and showroom glows, which have no city around
    // them to light, out of the set.

    // Fog for this draw is decided in one place further down, once remap_vertex_fog is known - see
    // the vertex-fog remap. Additive glows want it off for the same reason as the screen path (see
    // IsAdditiveGlow()), and that is folded into the same decision rather than being a second switch
    // the restore then has to undo.

    // Program the blend mode rather than inheriting whatever the last CPU-path draw left on the
    // device. An AlphaGlow texture is additive by definition and has no usable alpha channel left
    // to blend with (see above), so it gets One_One regardless of what agiCurState currently says.
    ApplyBlendSet(device, additive_glow ? agiBlendSet::One_One : agiCurState.GetBlendSet());

    // Hardware backface culling, exactly as the CPU path gets it. This used to be D3DCULL_NONE, on
    // the reasoning that DrawNativeTransform() had already excluded backfacing facets on the CPU
    // via plane equations so no winding convention was available. Both halves of that are wrong.
    //
    // The CPU plane test is agiMeshSet::IsBackfacing(), and it opens with
    //     return AllowEyeBackfacing && ...
    // AllowEyeBackfacing (agiworld/meshrend.cpp) initialises to *false* and is only raised by
    // agiMeshSet::InitMtx (game.asm ~324504), which does so only when all three of: the caller
    // passed planes && SurfaceCount > 1; MirrorMode is clear; and the transform passes InitMtx's
    // scale tolerance check. It is set straight back to 0 on every other path (~324569). So for a
    // large share of draws IsBackfacing() returns false for every facet and the "already culled on
    // the CPU" premise simply does not hold - nothing was culled.
    //
    // That was survivable on the CPU path because it never relied on the plane test alone: it feeds
    // its screen-space triangles through FlushState(), which programs D3DRS_CULLMODE from
    // agiCurState - agiRasterizer's constructor sets agiCullMode::CCW (agi/rsys.cpp) - so the GPU
    // culled by winding as a second line of defence. Forcing D3DCULL_NONE here removed that second
    // line and left the world path drawing every back face in the scene. That is the direct cause
    // of the reported artifacts: interior/rear building walls rasterising over their own front
    // faces (the "black triangle spots" and the "overlapping UV" look, which is a back face showing
    // its texture mirrored), and coplanar road surfaces fighting their own reverse side (the road
    // z-fighting) - none of which the CPU path exhibits, because it culls them.
    //
    // The winding is the same on both paths. The facet-to-triangle order here matches ClipTri()'s
    // exactly (quads split 1,2,3 / 1,3,0), and InitViewport() maps NDC to screen with
    // HalfWidth = +w/2 and HalfHeight = -h/2 - the same X-positive, Y-flipped mapping D3D9's own
    // viewport transform applies to our projected output. Same triangles, same screen-space
    // orientation, so the same cull mode is correct.
    //
    // Taking it from agiCurState rather than hardcoding CCW keeps the two paths in lockstep through
    // the places that deliberately disable culling for two-sided geometry (agiMeshSet::DrawWideLines
    // and mmShard::Draw both set agiCullMode::None around their draws).
    // Flipped - see ToD3DCullFlipped(). agiMeshSet::FlipX needs no extra compensation here even
    // though BuildProjectionMatrix() negates _11 for it: the CPU path mirrors the same way (by
    // negating InitViewport's HalfWidth) and likewise leaves its cull mode alone, so both paths
    // reverse together and stay in agreement.
    // -nocull forces both sides through. A path tracer needs closed shells: a back face it never
    // receives is a hole that light leaks through, and the interior of every building in the city is
    // exactly that shape.
    WorldSetRenderState(
        device, D3DRS_CULLMODE, agiNoCullEnabled() ? D3DCULL_NONE : ToD3DCullFlipped(agiCurState.GetCullMode()));

    // D3D9 fixed-function lighting does not renormalise normals after the world/view transform, and
    // agiWorldVtx::normal arrives as a unit vector from UnpackNormal[] in *model* space. Any
    // instance whose world matrix carries scale therefore hands the lighting pipeline a normal
    // whose length is no longer 1, and D3D9's N.L falls straight out of it - so the surface reads
    // uniformly too bright or too dark depending on the scale factor, and the specular term (which
    // is driven by a power of that dot product) exaggerates it further. Scaled instances are known
    // to exist here: InitMtx's guard for AllowEyeBackfacing is precisely a scale-tolerance test on
    // the current transform, which would be pointless if every transform were rigid.
    WorldSetRenderState(device, D3DRS_NORMALIZENORMALS, TRUE);

    // Vertex fog does not exist on this path and has to be re-expressed as table fog.
    //
    // agiFogMode::Vertex means "the fog factor is already baked into each vertex's specular alpha"
    // - FirstPass() computes it on the CPU from agiMeshSet::FogValue, and FlushState() programs
    // FOGTABLEMODE = FOGVERTEXMODE = NONE so D3D9 reads that alpha directly. But this path's FVF is
    // D3DFVF_XYZ|NORMAL|DIFFUSE|TEX1: agiWorldVtx carries no specular component at all, so D3D9
    // reads a fog factor of zero for every vertex and renders the entire submission in flat fog
    // colour. Whole city blocks turn into featureless sky-coloured silhouettes.
    //
    // This is not the default configuration - UsePixelFog is derived from dxiRendererInfo_t's
    // SpecialFlags bit 0x10 (game.asm ~177811), which agisdl/sdlsetup.cpp now sets for D3D9, so
    // mmCullCity::Cull() normally selects agiFogMode::Pixel. It is reachable though: UsePixelFog is
    // a registered debug-menu tweakable, and it is what every non-pixel-fog renderer uses.
    //
    // Ask the GPU for the same curve instead. agiMeshSet::SetFog() stores FogValue = 255/fog_end,
    // so fog_end recovers exactly, and D3D9 linear table fog over view-space depth reproduces the
    // CPU's per-vertex ramp (better, in fact - it interpolates per pixel).
    const bool remap_vertex_fog = (agiLastState.FogMode == agiFogMode::Vertex);

    // One decision, covering the remap, the additive-glow suppression and the ordinary case. It used
    // to be two conditional switches undone by the restore; with the restore deferred past the next
    // world draw, whatever a glow or a remap left behind would carry into it.
    bool fog_enable = (agiLastState.FogMode != agiFogMode::None) && !additive_glow;
    const bool want_fog_table = remap_vertex_fog && !additive_glow && (agiMeshSet::FogValue > 0.0f);

    // D3DRS_FOGTABLEMODE IS ONLY THIS PATH'S TO WRITE WHILE IT IS REMAPPING VERTEX FOG. Outside
    // that, FlushState() owns it and has already programmed it for the fog mode actually in use.
    //
    // Writing it unconditionally is what turned the whole city into a flat sheet of fog colour.
    // agiFogMode::Pixel is the normal mode here (agisdl/sdlsetup.cpp sets the SpecialFlags bit that
    // makes mmCullCity::Cull select it), and for Pixel this is not a remap - so an unconditional
    // write put D3DFOG_NONE over the D3DFOG_LINEAR that FlushState had just set. With FOGVERTEXMODE
    // also NONE and FOGENABLE on, "both modes NONE" means something specific in D3D9: take the fog
    // factor from the vertex specular alpha. This path's FVF is XYZ|NORMAL|DIFFUSE|TEX1 - agiWorldVtx
    // carries no specular at all - so every vertex read a factor of 0 and every world pixel came out
    // 100% fog colour. The HUD, text and minimap were untouched, because agiScreenVtx does carry
    // specular and the screen path never comes through here.
    if (remap_vertex_fog)
    {
        if (want_fog_table)
        {
            constexpr f32 kFogStart = 1.0f;
            const f32 fog_end = 255.0f / agiMeshSet::FogValue;

            WorldSetRenderState(device, D3DRS_FOGSTART, *reinterpret_cast<const DWORD*>(&kFogStart));
            WorldSetRenderState(device, D3DRS_FOGEND, *reinterpret_cast<const DWORD*>(&fog_end));
            WorldSetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
        }
        else
        {
            // Either an additive glow, which wants no fog at all, or no usable range to rebuild the
            // ramp from - and drawing unfogged is a far smaller error than drawing in solid fog
            // colour. Put the table mode back to what the screen path expects for vertex fog either
            // way, so a following draw in this same run does not inherit the branch above.
            WorldSetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);

            if (!additive_glow)
                fog_enable = false;
        }
    }

    // Under RTX Remix, world draws carry no fixed-function fog unless asked for. See
    // agiDX9WorldFogWanted() - Remix turns this state into its own volumetric fog and the city
    // comes out washed white.
    if (!agiDX9WorldFogWanted())
        fog_enable = false;

    WorldSetRenderState(device, D3DRS_FOGENABLE, fog_enable ? TRUE : FALSE);

    D3DMATRIX world_mat = ToD3DMatrix(world);

    // The CPU pretransform path (agiMeshSet::ARTS_TRANSFORM_DOT, meshrend.cpp) doesn't feed the
    // engine's View matrix to anything hardware-facing - it only ever uses the shared, closed-
    // Init()-refreshed `M` matrix, which was confirmed (by direct comparison against
    // Dot(World, View)) to negate the Z contribution of View relative to this raw Matrix34: M's
    // Z-row is exactly -1 times combined(World, View)'s Z-row, everywhere. That negation converts
    // this engine's view-space convention (camera looks down -Z) into the positive-forward
    // convention agiMeshSet::ToScreen()'s perspective divide (and every renderer built on it,
    // including OpenGL) expects. Feeding `view` to SetTransform(D3DTS_VIEW) unmodified hands D3D9's
    // hardware transform pipeline the *unflipped* convention, so its own view-space Z comes out
    // with the opposite sign from what the rest of the engine (and this same draw's projection
    // math below, tuned to match the CPU path) assumes - corrupting depth-adjacent geometry while
    // still submitting successfully (no error, just wrong). Negate View's Z-row here to match.
    Matrix34 view_zflip = view;
    view_zflip.m0.z = -view_zflip.m0.z;
    view_zflip.m1.z = -view_zflip.m1.z;
    view_zflip.m2.z = -view_zflip.m2.z;
    view_zflip.m3.z = -view_zflip.m3.z;

    // ---------------------------------------------------------------------------------------
    // Pathway B fork.
    //
    // Everything above this point is shared: texture bind, alpha/blend/cull/depth are output-merger
    // and sampler state that a pixel shader does not replace. Only the transform-and-light half of
    // the pipeline differs, so that is all that forks here.
    // ---------------------------------------------------------------------------------------------

    // -d3d9nofx. Resolved once so both branches below agree.
    const bool material_fx = fx && (fx->ReflectionTexture || fx->EnvTexture) && !PARAM_d3d9_nofx.get_or(false);

    // Does this draw need the matrix palette, or just a world matrix? A palette of one is the
    // latter - see the transform block below.
    const bool blend = skin && (skin->Count > 1);

    // A skinned submission always takes the fixed-function branch, whether it blends or not. The
    // matrix palette IS a fixed-function feature, and the programmable path's shaders transform by
    // a single world matrix with no notion of a bone; sending a pedestrian there would put its
    // limbs back in bind pose. The branch is self-contained - it programs its own transforms,
    // lights, material and FVF, and the shader is unbound between draws - so taking it while a
    // world shader exists is correct rather than merely safe.
    agiDX9WorldShader* shader = skin ? nullptr : Pipe()->WorldShader();

    if (shader)
    {
        agiDX9WorldDrawInfo info {};
        info.World = &world;
        info.ViewZFlip = &view_zflip;
        info.Proj = &proj_params;
        info.Texture = native_tex;
        info.Lit = hardware_lighting;
        info.StaticLighting = static_lighting;

        shader->Setup(device, info);

        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertex_count, index_count / 3, indices, D3DFMT_INDEX16,
            vertices, sizeof(agiWorldVtx));

        // Back to fixed function before anything else touches the device. The reflection pass below
        // and every pretransformed draw after this point are FF, and a left-over vertex shader
        // would reinterpret their XYZRHW vertices as model-space positions.
        shader->Unbind(device);

        if (material_fx)
        {
            DrawVehicleReflectionPass(
                device, vertices, vertex_count, indices, index_count, world, view, view_zflip, *fx, Pipe()->PerPixel());
            DrawGroundEnvPass(device, vertices, vertex_count, indices, index_count, world, *fx);

            // As on the fixed-function branch below - these write render and stage state straight to
            // the device, so the cache no longer answers for it.
            current_texture_ = nullptr;
            agiDX9InvalidateStateCache();
        }

        // The shader itself was bound and unbound outside the cache as well.
        agiDX9InvalidateStateCache();

        world_state_active_ = true;
        world_remap_vertex_fog_ = remap_vertex_fog;
        return true;
    }

    // WHICH VIEW MATRIX RTX REMIX GETS.
    //
    // view_zflip negates View's Z column, which makes the matrix handed to D3D9 a REFLECTION -
    // determinant -1 - rather than a rigid transform. The raster is correct, because the projection
    // below is tuned to match, but it is the one thing about this camera that differs from what
    // every other D3D9 game hands Remix, and Remix does reason about handedness (util_matrix.h has
    // isMirrorTransform, and its camera code recovers a basis from the inverse). The basis it
    // recovers from a mirrored view has its forward vector negated.
    //
    // -d3d9rhview hands it View unmodified and folds the flip into the projection instead. Clip
    // space is bit-identical either way (the derivation is at BuildProjectionMatrix), so this is an
    // A/B switch for that hypothesis, not a rendering change.
    //
    // Off by default because D3DTS_VIEW is not only consumed by the transform. D3D9 fixed-function
    // SPECULAR builds its eye vector assuming +Z is forward in view space, which is true of
    // view_zflip and not of View; and D3DTSS_TCI_CAMERASPACENORMAL texgen generates through
    // whatever is set here (see dx9ffshade.cpp). Static-rig specular and -ffperpixel are both off by
    // default, and the sphere-map pass takes view_zflip as an explicit argument rather than reading
    // the device, so the default configuration is unaffected - but the dynamic rig's specular on
    // vehicles is the thing to look at if this is turned on.
    const bool rh_view = PARAM_d3d9_rhview.get_or(false);

    D3DMATRIX view_mat = ToD3DMatrix(rh_view ? view : view_zflip);
    D3DMATRIX proj_mat = BuildProjectionMatrix(proj_params, rh_view);

    WorldSetTransform(device, D3DTS_VIEW, view_mat);
    WorldSetTransform(device, D3DTS_PROJECTION, proj_mat);

    // D3DTS_WORLD and D3DTS_WORLDMATRIX(0) are the same slot, and DrawNativeTransform passes the
    // palette's first bone as `world`, so this is bone 0 for a skinned draw and the ordinary world
    // matrix for everything else. A palette of one needs nothing further: it is a plain world
    // transform, which is exactly what submitting a skinned model one bone at a time amounts to.
    WorldSetTransform(device, D3DTS_WORLD, world_mat);

    if (blend)
    {
        // Each entry already carries bone * world, so the hardware transform is (bind-pose vertex) *
        // bone * world * view * projection - term for term what the CPU skinner used to compute and
        // then throw away by projecting it.
        for (u32 i = 1; i < skin->Count; ++i)
        {
            D3DMATRIX bone_mat = ToD3DMatrix(skin->Bones[i]);
            device->SetTransform(D3DTS_WORLDMATRIX(i), &bone_mat);
        }
    }

    // D3DVBF_0WEIGHTS: no weights at all, one matrix per vertex named by its own index. The binding
    // is rigid, so there is nothing to blend and nothing to normalise - this is the degenerate case
    // of vertex blending, and the cheapest thing the vertex pipeline can do.
    //
    // Outside the `blend` branch and mirrored, because this is the one piece of state a skinned draw
    // leaves behind that corrupts rather than mis-shades what follows: with D3DVBF_0WEIGHTS still
    // set, D3D9 reads a matrix index out of a vertex layout that has no beta field and transforms by
    // whatever palette slot that lands on. The restore used to catch it after every draw; it no
    // longer runs between world draws, so a pedestrian would place the next building by a thigh
    // bone. Almost every draw is unskinned, so the mirror means this costs nothing in practice.
    WorldSetRenderState(device, D3DRS_INDEXEDVERTEXBLENDENABLE, blend ? TRUE : FALSE);
    WorldSetRenderState(device, D3DRS_VERTEXBLEND, blend ? D3DVBF_0WEIGHTS : D3DVBF_DISABLE);

    // Meshes loaded without MESH_SET_NORMAL carry no normals, so there is nothing for hardware
    // lighting to work from - their agiWorldVtx::normal is filler. The CPU path draws them
    // straight from their baked vertex colors, and this reproduces that exactly: lighting off, so
    // D3D9 takes vertex diffuse through unmodified. Only the transform moves to hardware, which is
    // the whole point - it puts this geometry (most static city scenery, and the low-detail LODs
    // that make up the far half of the view) into the world-space stream RTX Remix can use,
    // without changing a single pixel of how it is shaded.
    if (!hardware_lighting)
    {
        WorldSetRenderState(device, D3DRS_LIGHTING, FALSE);
        WorldSetRenderState(device, D3DRS_SPECULARENABLE, FALSE);
    }
    else
    {
        WorldSetRenderState(device, D3DRS_LIGHTING, TRUE);

        // Specular is legitimate for the dynamic rig - SetupD3D9Material() takes its specular
        // colour and power from the engine's own agiMtlDef (agiCurState.GetMtl()), i.e. real
        // authored material data for cars and movers. The static city rig has no specular concept
        // at all, so it only gets one when explicitly asked for. See SetupD3D9StaticMaterial().
        WorldSetRenderState(
            device, D3DRS_SPECULARENABLE, (!static_lighting || agiDX9WantsStaticSpecular()) ? TRUE : FALSE);

        WorldSetRenderState(device, D3DRS_COLORVERTEX, TRUE);
        WorldSetRenderState(device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);

        // Ambient must come from the vertex colour, not the material. agiMeshLighterTriple
        // (agiworld/meshlight.cpp, mmxTriple) computes
        //     intensity = key + fill1 + fill2 + ambient      (clamped to 1.0)
        //     output    = vertex_colour * intensity
        // i.e. ambient is *inside* the term the vertex colour multiplies. With D3DMCS_MATERIAL and
        // a material Ambient of (1,1,1,1), D3D9 instead computed
        //     output = vertex_colour * (key + fill1 + fill2) + ambient
        // adding the full, unmodulated ambient on top of every surface. That washes out exactly the
        // content whose baked vertex colours are dark - night-time building facades and the
        // ambient-occluded underside of geometry - and it is a flat additive lift, so it cannot be
        // dialled out by the ambient value alone. D3DMCS_COLOR1 puts ambient back inside the
        // product and matches the CPU rig term for term.
        WorldSetRenderState(device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
        WorldSetRenderState(device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
        WorldSetRenderState(device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    }

    // Fixed-function per-pixel Blinn-Phong for the sun. Only for the static city rig: the dynamic
    // rig (SetupD3D9Lights) is a list of up to eight arbitrary point and spot lights, and promoting
    // those would be one additive pass each. The sun is a single directional light shared by the
    // whole scene, which is what makes one extra pass per mesh a reasonable trade.
    agiDX9FFPerPixel* per_pixel =
        (hardware_lighting && static_lighting && agiDX9PerPixelEnabled()) ? Pipe()->PerPixel() : nullptr;

    if (!hardware_lighting)
    {
        // No light/material setup at all - nothing consumes it with lighting disabled.
    }
    else if (static_lighting)
    {
        const Vector3& ambient = agiMeshLighterAmbient;
        WorldSetRenderState(device, D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(ambient.x, ambient.y, ambient.z, 1.0f));

        SetupD3D9StaticLights(device, per_pixel != nullptr);
        SetupD3D9StaticMaterial(device);
    }
    else
    {
        const Vector3& scene_ambient = agiLighter::SceneAmbient;
        WorldSetRenderState(
            device, D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(scene_ambient.x, scene_ambient.y, scene_ambient.z, 1.0f));

        SetupD3D9Lights(device);
        SetupD3D9Material(device);
    }

    WorldSetFVF(device, blend ? kWorldSkinFVF : (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1));

    i32 primitive_count = index_count / 3;

    // -ghash. Immediately before the draw, which is where Remix would hash it, and on exactly the
    // bytes handed to the device - with one exception. A skinned draw submits DX9SkinVtx, which is
    // these bytes with a bone index spliced in after the position, so its true stride and layout
    // differ. What the diagnostic exists to answer is whether the submitted geometry is STABLE
    // frame to frame, and the spliced index is as stable as everything around it (it is a property
    // of the mesh's bone binding), so hashing the unspliced form answers the same question.
    // Skinned and unskinned submissions of one mesh are not comparable to each other, which no
    // caller does anyway.
    const u64 geometry_hash = agiDX9GHashRecord(vertices, vertex_count, indices, index_count, native_tex);

    // -ghashcolor. Replace the texture with a flat hash colour for this draw only.
    //
    // TFACTOR rather than a vertex-colour rewrite, because the vertex data is the very thing being
    // hashed and must not be touched - tinting through the vertices would change the hash it is
    // supposed to be visualising. Nothing else on this path uses D3DRS_TEXTUREFACTOR.
    //
    // No explicit restore: RestoreStateAfterWorldDraw() below already re-applies ApplyTexEnv() from
    // tex_env_, which puts COLOROP/COLORARG1 back, and the stale TEXTUREFACTOR value is only ever
    // read when a stage selects TFACTOR, which nothing else does.
    if (PARAM_ghashcolor.get_or(false))
    {
        WorldSetRenderState(device, D3DRS_TEXTUREFACTOR, agiDX9GHashColor(geometry_hash));
        WorldSetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        WorldSetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    }

    if (blend)
    {
        // ARTS_ALLOCA for the reason DrawNativeTransform documents at length: calling the engine's
        // allocator from inside a draw corrupts the simulation heap. A pedestrian is a few hundred
        // vertices, so this is a few tens of KB.
        DX9SkinVtx* skin_verts = ARTS_ALLOCA(DX9SkinVtx, vertex_count);

        BuildSkinVertices(skin_verts, vertices, vertex_count, skin->Slots);

        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertex_count, primitive_count, indices, D3DFMT_INDEX16,
            skin_verts, sizeof(DX9SkinVtx));
    }
    else
    {
        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertex_count, primitive_count, indices, D3DFMT_INDEX16,
            vertices, sizeof(agiWorldVtx));
    }

    // Per-fragment sun, added on top of the base pass's ambient + fills. Before the material second
    // passes, because chrome and the ground map composite over lit paint and lit road.
    //
    // Neither this nor the material passes below is reachable for a skinned draw - both resubmit
    // this draw's positions under their own vertex layout and their own single world matrix, which
    // a palette-skinned mesh does not have. per_pixel is gated on static_lighting, which a
    // pedestrian never has, and fx is null for every caller that skins; the guards say so rather
    // than leaving it to those two facts holding.
    if (per_pixel && !skin)
    {
        agiDX9Census.PerPixelPasses += per_pixel->DrawSunPasses(
            device, vertices, vertex_count, indices, index_count, sizeof(agiWorldVtx), native_handle, view_zflip);

        // Writes render and stage state straight to the device (dx9ffshade.cpp), so what the cache
        // remembers is no longer what the device holds.
        current_texture_ = nullptr;
        agiDX9InvalidateStateCache();
    }

    // Second passes, in the order the original composited them: chrome over the paint, ground map
    // over the road. Both reuse this draw's vertex positions and world matrix, so they are ordinary
    // world-space geometry rather than the CPU-pretransformed overlays they replace.
    if (material_fx && !skin)
    {
        DrawVehicleReflectionPass(
            device, vertices, vertex_count, indices, index_count, world, view, view_zflip, *fx, Pipe()->PerPixel());
        DrawGroundEnvPass(device, vertices, vertex_count, indices, index_count, world, *fx);

        // Same as the per-pixel passes above - their own pipeline, written straight to the device.
        current_texture_ = nullptr;
        agiDX9InvalidateStateCache();
    }

    // Deferred, not skipped. See LeaveWorldState() - the next CPU-pretransformed draw runs it, and
    // between two world draws there is nothing to put back for.
    world_state_active_ = true;
    world_remap_vertex_fog_ = remap_vertex_fog;

    return true;
}
