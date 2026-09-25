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

#include "dx9pipe.h"

#include "agi/cmodel.h"
#include "agi/error.h"
#include "agi/rsys.h"
#include "agirend/bilight.h"
#include "agirend/bilmodel.h"
#include "agirend/lighter.h"
#include "agirend/rdlp.h"
#include "agirend/zbrender.h"
#include "agiworld/cardworld.h"
#include "agiworld/glowlight.h"
#include "agiworld/meshrend.h"
#include "agiworld/skyenv.h"
#include "data7/utimer.h"
#include "eventq7/active.h"
#include "pcwindis/dxinit.h"
#include "pcwindis/setupdata.h"

#include "dx9bitmap.h"
#include "dx9context.h"
#include "dx9meshcache.h"
#include "dx9remix.h"
#include "dx9remixsky.h"
#include "dx9remixwet.h"
#include "dx9rsys.h"
#include "dx9texdef.h"
#include "dx9view.h"

#include "dx9_windows.h"

#include <SDL3/SDL_video.h>

define_dummy_symbol(agidx9_dx9pipe);

agiDX9Pipeline::agiDX9Pipeline() = default;
agiDX9Pipeline::~agiDX9Pipeline() = default;

// -d3d9shaders is deliberately no longer registered. It was the only way to select Pathway B, and
// Pathway B is unwired (see BeginGfx). Leaving the switch registered would advertise a setting that
// silently does nothing, which is worse than the flag being unrecognised.

// Renders the frame into an offscreen target and blits it back. Exercises the render-target
// framework end to end; also the hook a post-processing chain will attach to. Not Remix-compatible.
static mem::cmd_param PARAM_d3d9_scenetarget {"d3d9scenetarget", "Render the scene through an offscreen target"};

// The live D3D9 device between EndGfx() and the BeginGfx() that follows it, holding it alive across
// a pipeline restart. See EndGfx() for why the device must not simply be destroyed.
//
// Note this parks the *device*, not the agiDX9Context wrapping it, and that distinction is load
// bearing. A pipeline restart re-enters MainPhase and resets the engine's memory arena - the second
// init reports "MemStat: 'ARTS Early Init' 0K before" - so every arnew allocation, an agiDX9Context
// among them, is freed wholesale at that point. Parking the wrapper leaves a dangling pointer that
// faults the moment the next BeginGfx() touches it. An IDirect3DDevice9 is a COM object living in
// the D3D9 DLL's own heap, untouched by the arena reset, so it survives and the cheap wrapper is
// simply rebuilt around it.
static IDirect3DDevice9* s_parked_device = nullptr;

i32 agiDX9Pipeline::BeginGfx()
{
    if (gfx_started_)
        return AGI_ERROR_ALREADY_INITIALIZED;

    valid_bit_depths_ = 0x4;
    flags_ = 0x1 | 0x4 | 0x10;

    if (i32 error = agiSDLPipeline::BeginGfx())
    {
        return error;
    }

    SDL_GetWindowSizeInPixels(window_, &horz_res_, &vert_res_);
    Displayf("Window Resolution: %u x %u", horz_res_, vert_res_);

    // Adopts the parked device when there is one, resetting it to the new size, rather than building
    // a second device on the same window. See s_parked_device.
    dx9_context_ = arnew agiDX9Context(window_, static_cast<u32>(horz_res_), static_cast<u32>(vert_res_),
        !dxiIsFullScreen(), (device_flags_1_ & 0x1) != 0, s_parked_device);

    s_parked_device = nullptr;

    // The Remix API, once per process and only with -remixapi. Here rather than earlier because the
    // bridge binds API calls to the most recently created D3D9 device, so one has to exist first.
    // Before any texture is created, so every glow texture gets its colour grid (dx9texdef.cpp).
    agiDX9RemixApiInit();

    screen_format_ = agiSurfaceDesc::FromFormat(PixelFormat_A8R8G8B8);
    opaque_format_ = agiSurfaceDesc::FromFormat(PixelFormat_X8R8G8B8);
    alpha_format_ = agiSurfaceDesc::FromFormat(PixelFormat_A8R8G8B8);

    screen_color_model_ = as_rc agiColorModel::FindMatch(&screen_format_);
    opaque_color_model_ = as_rc agiColorModel::FindMatch(&opaque_format_);
    alpha_color_model_ = as_rc agiColorModel::FindMatch(&alpha_format_);
    text_color_model_ = alpha_color_model_;

    TexSearchPath = "tex16a\0tex16o\0tex16\0"_xconst;

    agiCurState.SetCullMode(agiCullMode::None);
    agiCurState.SetBlendSet(agiBlendSet::SrcAlpha_InvSrcAlpha);
    agiCurState.SetTexturePerspective(true);
    agiCurState.SetMaxTextures(1);
    agiCurState.SetSmoothShading(true);

    rasterizer_ = arnewr agiDX9Rasterizer(this);
    renderer_ = arnewr agiZBufRenderer(rasterizer_.get());

    // PATHWAY B IS UNWIRED. This is the one place it was ever turned on, and the call is gone.
    //
    // The work now goes into Pathway A, because Pathway A is the one RTX Remix can reconstruct a
    // scene from - a draw issued through a vertex shader is opaque to Remix, which cannot know what
    // the shader did to the position. Pathway B was pulling in the opposite direction.
    //
    // Nothing is deleted. agiDX9WorldShader, agiDX9SkyProbe, the HLSL in game/hlsl and the glow
    // harvest in agiworld/meshrend.cpp are all still here and still compile; they simply have no
    // caller. world_shader_ therefore never becomes valid, WorldShader() below returns nullptr for
    // the rest of the process, and every programmable branch downstream is dead code.
    //
    // What Pathway B *learned* - where this engine keeps its light sources, its sun, its materials
    // - is written up in docs/remix_api_data_sources.md, because that knowledge is what the Remix
    // API wants and it long outlives the renderer that discovered it.
    //
    // To re-wire, this and re-registering -d3d9shaders above is the whole switch:
    //     if (PARAM_d3d9_shaders.get_or(false))
    //         world_shader_.Init(dx9_context_->GetDevice());

    // Offscreen scene target. Off unless asked for, and deliberately so: routing the frame through
    // a texture and blitting it back is invisible to RTX Remix, which reconstructs from the draws
    // it sees rather than from the image (dx9target.h). It exists now because the render-target
    // framework is the thing shadows, bloom and environment probes are all waiting on, and a
    // framework nothing exercises is a framework nobody knows is broken - this path proves create,
    // bind, draw-into, unbind, sample, blit and survive-a-device-reset in one go.
    if (PARAM_d3d9_scenetarget.get_or(false))
    {
        scene_target_.Init(dx9_context_->GetDevice(), static_cast<u32>(horz_res_), static_cast<u32>(vert_res_));
    }

    // Fixed-function per-pixel Blinn-Phong (-ffperpixel). Not a shader path - it lights through the
    // texture-blending unit, which is the only part of fixed function that runs per fragment. Off by
    // default: it costs additive passes and, like anything that improves the raster image, it is
    // wasted under RTX Remix, which path-traces and discards the game's shading. See dx9ffshade.h.
    if (agiDX9PerPixelEnabled() || agiDX9PerPixelReflectEnabled())
    {
        per_pixel_.Init(dx9_context_->GetDevice());
    }

    // MaxTextures STAYS AT 1, even though the per-pixel passes bind two textures.
    //
    // Raising it looks harmless and is not: agiCurState::MaxTextures is not a statement about what
    // this backend can bind, it is a switch that changes the engine's own vertex layout. At > 1
    // agiPolySet::Init (agiworld/texsort.cpp) allocates agiScreenVtx2, which is 0x28 bytes against
    // agiScreenVtx's 0x20, while agiDX9Rasterizer::DrawMesh still submits with sizeof(agiScreenVtx)
    // as the stride - so every vertex after the first is read at the wrong offset and the whole
    // screen path renders progressively skewed. It also makes DrawLitEnv's MultiTexEnvMap branch
    // reachable for the first time, routing ground and road geometry into a CPU pretransform path
    // that had been dead code.
    //
    // Nothing here needs it. The per-pixel passes and the sphere/env second passes call SetTexture
    // and program their stage ops directly, and never consult GetMaxTextures(). Raising it is
    // A3's problem (the fixed-function env/sphere texgen described in the design doc), and A3 will
    // have to deal with agiScreenVtx2 before it can.

    InitScaling();
    InitScreenScale();

    gfx_started_ = true;

    return AGI_ERROR_SUCCESS;
}

// See the PRESENTATION TRANSFORM note in dx9pipe.h for what this is for.
void agiDX9Pipeline::InitScreenScale()
{
    screen_scale_x_ = 1.0f;
    screen_scale_y_ = 1.0f;
    screen_scale_active_ = false;

    // InitScaling() has already resolved -scaling into a destination rectangle on the backbuffer,
    // including the aspect correction: at a 640x480 pipeline on a 1920x1080 display it produces
    // 1440x1080 at x=240, which is the 4:3 pillarbox this content wants and the same rectangle the
    // UI layout arrives at on its own when the pipeline runs at the display resolution.
    if ((width_ <= 0) || (height_ <= 0) || (blit_width_ <= 0) || (blit_height_ <= 0))
        return;

    screen_scale_x_ = static_cast<f32>(blit_width_) / static_cast<f32>(width_);
    screen_scale_y_ = static_cast<f32>(blit_height_) / static_cast<f32>(height_);

    screen_scale_active_ = (blit_width_ != width_) || (blit_height_ != height_) || blit_x_ || blit_y_;

    if (screen_scale_active_)
    {
        Displayf("D3D9: Presenting %ix%i logical pixels as %ix%i at %i,%i", width_, height_, blit_width_, blit_height_,
            blit_x_, blit_y_);
    }
}

void agiDX9Pipeline::MapScreenRect(i32& x, i32& y, i32& w, i32& h) const
{
    if (!screen_scale_active_)
        return;

    // Map both edges rather than scaling the width, so two rectangles that shared an edge in logical
    // pixels still share it afterwards instead of leaving a seam or overlapping by a pixel.
    const i32 left = blit_x_ + static_cast<i32>(std::lround(x * screen_scale_x_));
    const i32 top = blit_y_ + static_cast<i32>(std::lround(y * screen_scale_y_));
    const i32 right = blit_x_ + static_cast<i32>(std::lround((x + w) * screen_scale_x_));
    const i32 bottom = blit_y_ + static_cast<i32>(std::lround((y + h) * screen_scale_y_));

    x = left;
    y = top;
    w = right - left;
    h = bottom - top;
}

void agiDX9Pipeline::EndGfx()
{
    text_color_model_ = nullptr;
    screen_color_model_ = nullptr;
    opaque_color_model_ = nullptr;
    alpha_color_model_ = nullptr;

    renderer_ = nullptr;
    rasterizer_ = nullptr;

    // Before the context: these are device objects and must not outlive the device.
    world_shader_.Shutdown();
    per_pixel_.Shutdown();
    scene_target_.Shutdown();

    // Drop both light registries. Neither is owned by this pipeline, and both hold pointers that
    // are about to become dangling: agiGlowLights[] borrows an agiTexDef* per harvested light, and
    // agiLighter::LIGHTS holds agiLight* registered by DeclareLight(). Quitting a race back to the
    // menu tears the pipeline down and then resets the engine's memory arena, which frees every one
    // of those objects wholesale without running a destructor - so RemoveLight() never gets to
    // compact LIGHTS, and nothing at all trims the glow registry.
    //
    // The next BeginFrame() then walks both from agiDX9WorldShader::UpdateLights() -> BuildLightPool
    // and faults on the first one it dereferences. That is the "ABORT: Exception caught during init"
    // on Quit to Race Menu, seen as ACCESS_VIOLATION in BuildLightPool with mmLoader::Init below it
    // on the stack. It needs the programmable path to be enabled, which is why it only shows up with
    // d3d9shaders = 1.
    //
    // Both describe a city that no longer exists, so clearing them loses nothing.
    //
    // The Remix API's lights go first, for the same reason: they are the runtime's copies of the
    // glow registry's entries. Destroying them here, while the parked device keeps the bridge alive,
    // is what stops the last race's street lamps lighting the menu.
    agiDX9RemixApiReleaseAll();
    agiResetGlowLights();

    // The race's time and weather belong to the city too (agiworld/skyenv.h). The next city
    // publishes its own; until then the Remix sky has nothing to drive, and gives the player's
    // "Textured Sky" setting back.
    agiSkyEnv.TimeOfDay = -1;
    agiSkyEnv.Weather = -1;
    agiDX9RemixSkyEndGfx();
    agiDX9RemixWetEndGfx();

    // Device buffers, and geometry of a city that is about to be freed. Before the context, which
    // parks the device the buffers belong to.
    agiDX9MeshCacheReleaseAll();

    // Same hazard, same reason: this borrows mmCullCity's sphere map, and the arena reset frees the
    // whole city underneath it. mmCullCity::Cull() republishes it for the next city.
    agiNativeCitySphereMap = nullptr;

    agiLighter::Current = 0;

    for (i32 i = 0; i < agiLighter::MAX_LIGHTS; ++i)
        agiLighter::LIGHTS[i] = nullptr;

    // Park the device rather than destroying it - the reference moves to s_parked_device and the
    // next BeginGfx() adopts it. Changing resolution (the 640x480 menu into a 1280x800 race) runs a
    // full EndGfx/BeginGfx cycle, and destroying a D3D9 device only to immediately create another
    // on the same HWND is what breaks RTX Remix: dxvk subclasses the window procedure, and that
    // subclass outlives the swapchain it refers to across the gap. The runtime says so on its way
    // down - "[D3D9WindowProc] Swapchain handle is invalid" - and the 64-bit bridge server then
    // faults, taking the game with it. The window itself is never recreated here (dxiWindowCreate
    // returns early when the renderer type is unchanged), so the device stays valid for it and a
    // Reset() to the new size is all that was ever needed.
    if (dx9_context_)
        s_parked_device = dx9_context_->DetachDevice();

    dx9_context_ = nullptr;

    gfx_started_ = false;
}

// NOTE: Not named "frameclear" - agigl/glpipe.cpp already declares a cmd_param with that name,
// and mem::cmd_param aborts the process if two instances ever share a name (both agigl and
// agidx9 are always linked together, so a name clash here is not hypothetical).
static mem::cmd_param PARAM_d3d9_frameclear {"d3d9frameclear"};

void agiDX9Pipeline::BeginFrame()
{
    ARTS_UTIMED(agiBeginFrame);

    // Age the live light set and retire lights whose sprite has not been drawn recently. See
    // agiworld/glowlight.h - lights persist across frames rather than being rebuilt, so a momentary
    // culling or LOD hiccup does not make them blink out. Only while something harvests into it
    // (the Remix API); otherwise the set is empty and ageing it is pure cost.
    if (agiGlowHarvestEnabled)
        agiUpdateGlowLights();

    remix_frame_sent_ = false;

    agiDX9RemixWetBeginFrame();

    // Makes room in the world mesh cache, if it needs it - the only point in a frame it may.
    agiDX9MeshCacheBeginFrame();

    // Keeps the game's sky dome off while RTX Remix Plus draws the sky. See dx9remixsky.cpp.
    agiDX9RemixSkyBeginFrame();

    agiPipeline::BeginFrame();

    if (!dx9_context_->BeginFrame())
        return;

    IDirect3DDevice9* device = dx9_context_->GetDevice();

    // Canonical depth range, re-asserted every frame. The other half of the projection-matrix fix
    // documented at length in BuildProjectionMatrix() (dx9rsys.cpp) - read that first; this is the
    // part that keeps the CPU pretransform path in step with it.
    //
    // BuildProjectionMatrix() now hardcodes the 0.5/0.5 NDC halving, because folding the engine's
    // depth guard band (DepthScale 0.495/0.499, minus ShadowZBias) into the matrix produces a
    // frustum with no far plane, which is what stops RTX Remix reconstructing a camera in gameplay.
    // The CPU pretransform path applies these same two globals per vertex in ToScreen(), so if they
    // stayed at the city's values the two paths would disagree about depth by up to 0.005 - enough,
    // at distance, to swap the depth order of world geometry and the screen-space effects drawn
    // against it (blob shadows, smoke, glow cards). Putting them back to 0.5/0.5 means both paths
    // write exactly the same depth for the same point, as they do today, only over the full range.
    //
    // Once per frame rather than once at startup because mmCullCity's constructor writes DepthScale
    // when a city loads (game.asm ~175726) and would otherwise silently undo this on the way into a
    // race - which is precisely when it matters.
    //
    // ShadowZBias is deliberately NOT touched. mmCullCity::Cull() subtracts it from DepthOffset for
    // the asPortalWeb pass and restores it afterwards; that still happens, the CPU path still honours
    // it, and the hardware path - whose projection no longer reads DepthOffset at all - simply does
    // not follow it down. The relative offset that leaves is 0.005 in the direction the bias exists
    // to create (shadow content in front of the surface it lies on), where before the two paths
    // moved together and the bias did nothing between them.
    if (!PARAM_d3d9_legacydepth.get_or(false))
    {
        agiMeshSet::DepthScale = 0.5f;
        agiMeshSet::DepthOffset = 0.5f;
    }

    // Bind the offscreen target before the clear, so the clear lands on it rather than on a
    // backbuffer that is about to be overwritten by the blit anyway.
    scene_target_bound_ = scene_target_.IsValid() && scene_target_.Begin(device);

    if (PARAM_d3d9_frameclear.get_or(true))
    {
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
    }

    // Rebuild the frame's point-light pool and cluster grid, now that agiUpdateGlowLights() above
    // has aged the registry. Once per frame, not once per draw: the pixel shader finds its own
    // lights by world position, so there is no per-mesh light selection left to do. See
    // agiDX9ClusterGrid in dx9shader.h.
    if (world_shader_.IsValid())
        world_shader_.UpdateLights(device);

    // D3D9 requires DrawPrimitive calls to happen between BeginScene/EndScene, but callers
    // (e.g. loading-screen/menu 2D drawing via asCullManager) don't always go through the
    // explicit BeginScene()/EndScene() pipeline calls below - those are only invoked when a
    // 3D camera is active. So the D3D9 scene has to span the *entire* frame here instead,
    // rather than being nested inside the (possibly skipped) BeginScene()/EndScene() pair.
    d3d_scene_active_ = SUCCEEDED(device->BeginScene());
}

void agiDX9Pipeline::BeginScene()
{
    ARTS_UTIMED(agiBeginScene);

    UpdateZTrick();

    agiPipeline::BeginScene();
    agiLighter::BeginScene();

    in_scene_ = true;

    renderer_->BeginGroup();
}

void agiDX9Pipeline::EndScene()
{
    ARTS_UTIMED(agiEndScene);

    // Last chance for world-space billboards and lines, before in_scene_ drops and the viewport is
    // reset below. agiTexSorter::Cull() flushes them in the normal case and this finds nothing to
    // do; it exists for the passes that never reach a Cull(alpha) - the showroom and the menus -
    // where a quad left queued would otherwise be drawn under the next pass's view, or counted as
    // HUD, or drawn against the full backbuffer instead of its own rectangle.
    agiFlushWorldQuads();

    rasterizer_->EndGroup();

    in_scene_ = false;

    // agiDX9Viewport::Activate() now sets the device viewport, so whichever agiViewport rendered
    // last leaves its rectangle on the device - and the HUD, text and minimap are all drawn after
    // this point (see the census note in EndFrame). Restore the full backbuffer so a sub-viewport
    // pass, notably the rear view mirror at 0.75/0.0 0.25x0.25, cannot clip them.
    D3DVIEWPORT9 viewport {};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = static_cast<DWORD>(horz_res_);
    viewport.Height = static_cast<DWORD>(vert_res_);
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    dx9_context_->GetDevice()->SetViewport(&viewport);

    agiPipeline::EndScene();

    // The 3D part of the frame is over - every camera, the rear view mirror included, has drawn, so
    // every glow has refreshed its registry slot - and the HUD is still to come. See SendRemixFrame.
    SendRemixFrame();
}

// Sends this frame's Remix API lights and sky, once per frame, at the end of the 3D scene.
//
// NOT AT THE END OF THE FRAME. Remix path traces the frame the moment it meets its first UI draw
// (d3d9_rtx.cpp, isRenderingUI: an orthographic projection with depth writes off, or a texture in
// rtx.uiTextures - and tagging the HUD's textures as UI is a normal part of setting a game up), and
// only falls back to Present when there is none. Everything sent after that point is applied to the
// NEXT frame. asCullManager::Update draws all the 3D inside one BeginScene/EndScene and the HUD,
// text and minimap after it, so lights sent from EndFrame arrived one frame late whenever the HUD
// triggered the path trace: every light one frame's travel behind its lamp, which at speed is a
// metre or more - the "lights fall behind when you speed up" that survived the tracking fixes.
//
// EndScene is the last point where everything the lights describe has been drawn and nothing that
// can trigger the path trace has. EndFrame still calls this, for frames with no scene at all.
void agiDX9Pipeline::SendRemixFrame()
{
    if (std::exchange(remix_frame_sent_, true))
        return;

    agiDX9RemixApiSubmitFrame();
    agiDX9RemixSkyEndFrame();
}

void agiDX9Pipeline::EndFrame()
{
    ARTS_UTIMED(agiEndFrame);

    IDirect3DDevice9* device = dx9_context_->GetDevice();

    // Resolve the offscreen target back to the backbuffer before anything reads the frame.
    //
    // The scene is ended first and a fresh one opened for the blit, rather than swapping targets
    // mid-scene. D3D9 is lenient about SetRenderTarget inside BeginScene/EndScene and some drivers
    // are not; the extra pair costs nothing once a frame and removes the question.
    //
    // This is where a post-processing chain goes: bind a shader, blit through it, repeat. The quad
    // does not care what is bound (see agiDX9BlitFullscreen).
    if (scene_target_bound_)
    {
        scene_target_bound_ = false;

        if (std::exchange(d3d_scene_active_, false))
            device->EndScene();

        scene_target_.End(device);

        if (SUCCEEDED(device->BeginScene()))
        {
            agiDX9BlitFullscreen(
                device, scene_target_.GetTexture(), static_cast<u32>(horz_res_), static_cast<u32>(vert_res_));

            device->EndScene();
        }

        // The blit wrote render, sampler and texture-stage state straight to the device, behind the
        // agiCurState/agiLastState pair's back. Poison the cache so the next frame re-issues
        // everything instead of trusting a stale record - the same hazard MeshWorld documents.
        agiLastState.Reset();

        // And behind agiDX9Rasterizer's render-state cache too, which answers for the device on the
        // basis that every write inside the rasterizer goes through it. This one did not.
        agiDX9InvalidateStateCache();

        // The blit also left stage 0 at POINT/CLAMP. The texture sampler mirror still holds the
        // filter and address modes of the last world texture, so without this it would skip the
        // writes that put them back, and the next frame would sample every texture with the
        // blit's settings.
        agiDX9InvalidateSamplerCache();
    }

    if (ScreenShotRequested())
        SaveScreenShot(CaptureScreen());

    if (std::exchange(d3d_scene_active_, false))
        device->EndScene();

    // Normally already sent from EndScene - see SendRemixFrame. This covers a frame without a scene.
    SendRemixFrame();

    dx9_context_->Present();

    // Submission census. Reports how much of the frame actually went out as world-space geometry
    // (model-space vertices + real SetTransform calls, which is what RTX Remix reconstructs from)
    // versus CPU-pretransformed XYZRHW screen-space triangles, which carry no world information.
    // Sampled every 120th frame so it stays readable rather than one line per frame.
    {
        static u32 census_frames = 0;

        if ((++census_frames % 120) == 0)
        {
            u32 world = agiDX9Census.WorldTris;
            u32 screen = agiDX9Census.ScreenTris;
            u32 total = world + screen;

            // in-scene screen triangles are the ones that still matter: real 3D content that
            // never got a world transform. The rest is HUD/text/minimap and is meant to be 2D.
            u32 screen_3d = agiDX9Census.ScreenTrisInScene;
            u32 total_3d = world + screen_3d;

            Displayf("DX9 CENSUS: frame=%u world=%u tris in %u calls (%u static-lit, %u unlit) | screen=%u tris in %u "
                     "calls, of which IN-SCENE(3D)=%u tris in %u calls | lines=%u in %u calls | "
                     "glowlights=%u live, %u pooled, %u cell slots | "
                     "cards=%u seen (%u no-tex, %u not-glow, %u harvested) | "
                     "worldquads=%u drawn, %u DROPPED | "
                     "normals=%u/%u draws flat, %u/%u tris flat | "
                     "reflect=%u drawn, %u no-normals, citysph=%s | perpixel=%u passes | "
                     "world share(3D only)=%.1f%% | world share(all)=%.1f%% | tris/call world=%.1f",
                census_frames, world, agiDX9Census.WorldCalls, agiDX9Census.WorldStaticLitTris,
                agiDX9Census.WorldUnlitTris, screen, agiDX9Census.ScreenCalls, screen_3d,
                agiDX9Census.ScreenCallsInScene, agiDX9Census.ScreenLines, agiDX9Census.ScreenLineCalls,
                agiGlowLightCount, world_shader_.LightCount(), world_shader_.CellFill(), agiGlowCardsSeen,
                agiGlowCardsNoTexture, agiGlowCardsNotGlow, agiGlowCardsHarvested, agiWorldQuadsDrawn,
                agiWorldQuadsDropped, agiMeshNormalDrawsFlat, agiMeshNormalDraws, agiMeshNormalTrisFlat,
                agiMeshNormalTris, agiReflectDraws, agiReflectSkipNoNormals, agiNativeCitySphereMap ? "yes" : "NULL",
                agiDX9Census.PerPixelPasses, total_3d ? (100.0 * world / total_3d) : 0.0,
                total ? (100.0 * world / total) : 0.0,
                agiDX9Census.WorldCalls ? (1.0 * world / agiDX9Census.WorldCalls) : 0.0);

            // -ghash, on its own line and only when the switch is on. CHURN is the number that
            // matters: distinct hashes appearing for the first time this frame. On a settled view
            // it should fall to zero, because MeshWorld submits model-space vertices and the world
            // matrix goes out through SetTransform rather than into the vertex bytes. Churn that
            // never settles means something rewrites geometry every frame, and Remix mesh
            // replacements cannot stick to it.
            //
            // Read CHURN together with table=used/capacity and wraps. CHURN alone was ambiguous:
            // the table used to stop counting once full, so a saturated table and a genuinely
            // stable scene both reported zero. See the note at the fill check in agiDX9GHashRecord.
            if (agiDX9Census.GHashDraws)
            {
                Displayf("DX9 GHASH: frame=%u draws=%u distinct=%u stable=%u CHURN(new)=%u | table=%u/%u wraps=%u",
                    census_frames, agiDX9Census.GHashDraws, agiDX9Census.GHashDistinct, agiDX9Census.GHashStable,
                    agiDX9Census.GHashNew, agiDX9GHashTableUsed(), agiDX9GHashTableCapacity(), agiDX9GHashWraps());
            }

            agiDX9DumpAttribution();
            agiDX9RemixApiLogStats(census_frames);
            agiDX9RemixWetLogStats(census_frames);
            // Where the submitted vertex normals came from. See -geonormals in agiworld/meshrend.cpp.
            // "against stored" should stay at or near zero: it counts meshes whose own stored normals
            // disagreed with the winding the rebuild takes as outward.
            Displayf("DX9 NORMALS: frame=%u draws without stored normals=%u/%u | since last: rebuilt from geometry=%u "
                     "meshes (%u against stored, %u too large)",
                census_frames, agiMeshNormalDrawsFlat, agiMeshNormalDraws, agiMeshGeoNormalBuilds,
                agiMeshGeoNormalFlips, agiMeshGeoNormalSkipped);

            agiMeshGeoNormalBuilds = 0;
            agiMeshGeoNormalFlips = 0;
            agiMeshGeoNormalSkipped = 0;

            agiDX9MeshCacheLogStats(census_frames, agiDX9Census.WorldCachedCalls, agiDX9Census.WorldCalls);
        }

        agiDX9Census = {};
        agiResetWorldQuadStats();
        agiResetMeshNormalStats();
        agiResetReflectStats();
        agiDX9GHashNextFrame();
    }

    agiPipeline::EndFrame();
}

RcOwner<agiTexDef> agiDX9Pipeline::CreateTexDef()
{
    return as_owner arnewr agiDX9TexDef(this);
}

RcOwner<agiTexLut> agiDX9Pipeline::CreateTexLut()
{
    return nullptr;
}

RcOwner<DLP> agiDX9Pipeline::CreateDLP()
{
    return as_owner arnewr RDLP(this);
}

RcOwner<agiLight> agiDX9Pipeline::CreateLight()
{
    return as_owner arnewr agiBILight(this);
}

RcOwner<agiLightModel> agiDX9Pipeline::CreateLightModel()
{
    return as_owner arnewr agiBILightModel(this);
}

RcOwner<agiViewport> agiDX9Pipeline::CreateViewport()
{
    return as_owner arnewr agiDX9Viewport(this);
}

RcOwner<agiBitmap> agiDX9Pipeline::CreateBitmap()
{
    return as_owner arnewr agiDX9Bitmap(this);
}

void agiDX9Pipeline::CopyBitmap(i32 dst_x, i32 dst_y, agiBitmap* src, i32 src_x, i32 src_y, i32 width, i32 height)
{
    if (!IsAppActive())
        return;

    // FIXME: https://github.com/0x1F9F1/Open1560/issues/22
    if (src_y + height > src->GetHeight())
        return;

#ifdef ARTS_DEV_BUILD
    ++agiBitmapCount;
    agiBitmapPixels += width * height;
#endif

    agiTexDef* texture = static_cast<agiDX9Bitmap*>(src)->GetHandle();

    bool debug_draw = agiCurState.GetDrawMode() == agiDrawDepth;

    auto old_tex = agiCurState.SetTexture(debug_draw ? nullptr : texture);
    auto old_draw_mode = agiCurState.SetDrawMode(agiDrawTextured);
    auto old_depth = agiCurState.SetZEnable(false);
    auto old_zwrite = agiCurState.SetZWrite(false);
    auto old_alpha = agiCurState.SetAlphaEnable(debug_draw ? true : false);
    auto old_filter = agiCurState.SetTexFilter(agiTexFilter::Point);
    auto old_fog_mode = agiCurState.SetFogMode(agiFogMode::None);
    auto old_fog_color = agiCurState.SetFogColor(0x00000000);
    auto old_blend_set = agiCurState.SetBlendSet(debug_draw ? agiBlendSet::One_One : agiBlendSet::SrcAlpha_InvSrcAlpha);

    agiScreenVtx blank {0.0f, 0.0f, 0.0f, 1.0f, debug_draw ? 0xFF000044 : 0xFFFFFFFF, 0xFFFFFFFF, 0.0f, 0.0f};
    agiScreenVtx verts[4] {blank, blank, blank, blank};
    u16 indices[6] {0, 1, 3, 1, 2, 3};

    verts[3].x = verts[0].x = static_cast<f32>(dst_x);
    verts[1].y = verts[0].y = static_cast<f32>(dst_y);
    verts[3].tu = verts[0].tu = static_cast<f32>(src_x) / static_cast<f32>(src->GetWidth());
    verts[1].tv = verts[0].tv = static_cast<f32>(src_y) / static_cast<f32>(src->GetHeight());

    verts[1].x = verts[2].x = static_cast<f32>(dst_x + width);
    verts[3].y = verts[2].y = static_cast<f32>(dst_y + height);
    verts[1].tu = verts[2].tu = static_cast<f32>(src_x + width) / static_cast<f32>(src->GetWidth());
    verts[3].tv = verts[2].tv = static_cast<f32>(src_y + height) / static_cast<f32>(src->GetHeight());

    rasterizer_->Mesh(agiVtxType::Screen, (agiVtx*) verts, 4, indices, 6);

    agiCurState.SetTexture(old_tex);
    agiCurState.SetDrawMode(old_draw_mode);
    agiCurState.SetZEnable(old_depth);
    agiCurState.SetZWrite(old_zwrite);
    agiCurState.SetAlphaEnable(old_alpha);
    agiCurState.SetTexFilter(old_filter);
    agiCurState.SetFogMode(old_fog_mode);
    agiCurState.SetFogColor(old_fog_color);
    agiCurState.SetBlendSet(old_blend_set);
}

void agiDX9Pipeline::ClearAll(i32 color)
{
    u8 r = static_cast<u8>(color & 0xFF);
    u8 g = static_cast<u8>((color >> 8) & 0xFF);
    u8 b = static_cast<u8>((color >> 16) & 0xFF);

    dx9_context_->GetDevice()->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(r, g, b), 1.0f, 0);
}

void agiDX9Pipeline::ClearRect(i32 x, i32 y, i32 width, i32 height, u32 color)
{
    auto tex = agiCurState.SetTexture(nullptr);
    auto draw_mode = agiCurState.SetDrawMode(agiDrawTextured);
    auto depth = agiCurState.SetZEnable(false);
    auto zwrite = agiCurState.SetZWrite(false);
    auto alpha = agiCurState.SetAlphaEnable(false);
    auto filter = agiCurState.SetTexFilter(agiTexFilter::Point);
    auto fog_mode = agiCurState.SetFogMode(agiFogMode::None);
    auto fog_color = agiCurState.SetFogColor(0x00000000);

    agiScreenVtx blank {0.0f, 0.0f, 0.0f, 1.0f, color | 0xFF000000, 0xFFFFFFFF, 0.0f, 0.0f};
    agiScreenVtx verts[4] {blank, blank, blank, blank};
    u16 indices[6] {0, 1, 3, 1, 2, 3};

    verts[3].x = verts[0].x = static_cast<f32>(x);
    verts[1].y = verts[0].y = static_cast<f32>(y);

    verts[1].x = verts[2].x = static_cast<f32>(x + width);
    verts[3].y = verts[2].y = static_cast<f32>(y + height);

    rasterizer_->Mesh(agiVtxType::Screen, (agiVtx*) verts, 4, indices, 6);

    agiCurState.SetTexture(tex);
    agiCurState.SetDrawMode(draw_mode);
    agiCurState.SetZEnable(depth);
    agiCurState.SetZWrite(zwrite);
    agiCurState.SetAlphaEnable(alpha);
    agiCurState.SetTexFilter(filter);
    agiCurState.SetFogMode(fog_mode);
    agiCurState.SetFogColor(fog_color);
}

void agiDX9Pipeline::Init()
{
    // TODO: Properly use width/height/depth
    width_ = PARAM_width.get_or<i32>(640);
    height_ = PARAM_height.get_or<i32>(480);
    bit_depth_ = PARAM_depth.get_or<i32>(32);

    device_flags_1_ = 0x1032; // hal, zbuffer, vram

    if (PARAM_vsync.get_or(true))
        device_flags_1_ |= 0x1;

    device_flags_2_ = device_flags_1_;
    device_flags_3_ = device_flags_1_;

    PackShift = PARAM_pack.get_or<i32>(0);
    AnnotateTextures = PARAM_annotate.get_or(false);
}

Ptr<agiSurfaceDesc> agiDX9Pipeline::CaptureScreen()
{
    IDirect3DDevice9* device = dx9_context_->GetDevice();

    IDirect3DSurface9* backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);

    D3DSURFACE_DESC desc {};
    backbuffer->GetDesc(&desc);

    Ptr<agiSurfaceDesc> surface =
        as_ptr agiSurfaceDesc::Init(desc.Width, desc.Height, agiSurfaceDesc::FromFormat(PixelFormat_B8G8R8));

    IDirect3DSurface9* sysmem = nullptr;

    if (SUCCEEDED(device->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, nullptr)))
    {
        if (SUCCEEDED(device->GetRenderTargetData(backbuffer, sysmem)))
        {
            D3DLOCKED_RECT locked;

            if (SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                u8* dst = static_cast<u8*>(surface->Surface);
                const u8* src = static_cast<const u8*>(locked.pBits);

                for (u32 y = 0; y < desc.Height; ++y)
                {
                    const u32* src_row = reinterpret_cast<const u32*>(src + static_cast<usize>(y) * locked.Pitch);
                    u8* dst_row = dst + static_cast<usize>(y) * surface->Pitch;

                    for (u32 x = 0; x < desc.Width; ++x)
                    {
                        u32 pixel = src_row[x];
                        dst_row[x * 3 + 0] = static_cast<u8>(pixel & 0xFF);
                        dst_row[x * 3 + 1] = static_cast<u8>((pixel >> 8) & 0xFF);
                        dst_row[x * 3 + 2] = static_cast<u8>((pixel >> 16) & 0xFF);
                    }
                }

                sysmem->UnlockRect();
            }
        }

        sysmem->Release();
    }

    backbuffer->Release();

    return surface;
}

Owner<agiPipeline> dx9CreatePipeline([[maybe_unused]] i32 argc, [[maybe_unused]] char** argv)
{
    Ptr<agiDX9Pipeline> result = arnew agiDX9Pipeline();
    result->Init();
    return as_owner result;
}
