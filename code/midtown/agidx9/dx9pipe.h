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

#include "agisdl/sdlpipe.h"

#include "dx9ffshade.h"
#include "dx9shader.h"
#include "dx9target.h"

class agiDX9Context;
class agiDX9Rasterizer;

class agiDX9Pipeline final : public agiSDLPipeline
{
public:
    agiDX9Pipeline();
    ~agiDX9Pipeline();

    i32 BeginGfx() override;
    void EndGfx() override;

    void BeginFrame() override;
    void BeginScene() override;
    void EndScene() override;
    void EndFrame() override;

    RcOwner<agiTexDef> CreateTexDef() override;
    RcOwner<agiTexLut> CreateTexLut() override;
    RcOwner<DLP> CreateDLP() override;
    RcOwner<agiLight> CreateLight() override;
    RcOwner<agiLightModel> CreateLightModel() override;
    RcOwner<agiViewport> CreateViewport() override;
    RcOwner<agiBitmap> CreateBitmap() override;

    void CopyBitmap(i32 dst_x, i32 dst_y, agiBitmap* src, i32 src_x, i32 src_y, i32 width, i32 height) override;

    void ClearAll(i32 color) override;
    void ClearRect(i32 x, i32 y, i32 width, i32 height, u32 color) override;

    bool SupportsNativeTransform() const override
    {
        return true;
    }

    // Whether we are between BeginScene() and EndScene(), i.e. drawing the 3D world rather than
    // the HUD/menu overlay. Used by the submission census to tell real world geometry that is
    // still CPU-pretransformed apart from genuinely-2D interface drawing.
    bool IsInScene() const
    {
        return in_scene_ != 0;
    }

    void Init();

    agiDX9Context* Context() const
    {
        return dx9_context_.get();
    }

    agiDX9Rasterizer* Rast()
    {
        return rasterizer_.get();
    }

    // Pathway B. Returns null until BeginGfx() has successfully brought it up, and stays null
    // whenever -d3d9shaders is absent, the device lacks ps_3_0, or a shader failed to compile - so
    // every caller treats "no shader" as "use the fixed-function path", which is also the default.
    agiDX9WorldShader* WorldShader()
    {
        return world_shader_.IsValid() ? &world_shader_ : nullptr;
    }

    // The frame's offscreen colour target, or null when the scene renders straight to the
    // backbuffer (the default, and the only Remix-compatible arrangement). Shadow maps and
    // post-processing passes get their own targets; this one exists so the framework is exercised
    // by something real rather than only by code that does not exist yet.
    agiDX9RenderTarget* SceneTarget()
    {
        return scene_target_.IsValid() ? &scene_target_ : nullptr;
    }

    // Fixed-function per-pixel Blinn-Phong. Null unless -ffperpixel is set and the device can do
    // DOT3 with per-stage constants. Nothing to do with Pathway B - this is the texture-blending
    // unit, not a shader - but like Pathway B it is off by default and not for use with Remix.
    agiDX9FFPerPixel* PerPixel()
    {
        return per_pixel_.IsValid() ? &per_pixel_ : nullptr;
    }

    // PRESENTATION TRANSFORM: logical pixels -> backbuffer pixels.
    //
    // The engine draws in a coordinate space of agiPipeline::width_ x height_ ("logical pixels"):
    // agiViewport rectangles are fractions of it, and every CPU-pretransformed agiScreenVtx carries
    // an absolute position in it. This backend's backbuffer is a different size - BeginGfx() builds
    // the device at the *window* size (horz_res_/vert_res_), which in fullscreen is the whole
    // display no matter what the pipeline resolution is. Those two disagree whenever the pipeline is
    // not running at the desktop resolution, and in the menus they ALWAYS disagree, because
    // CreatePipeline() (midtown.cpp) hardcodes SetRes(640, 480) for the menu game state.
    //
    // Nothing used to close that gap, so logical pixels were written to the backbuffer one-for-one:
    // a 640x480 menu occupied the top-left ninth of a 1920x1080 screen with black around it, and the
    // showroom's car viewport - a fraction of 640x480 - became a small box beside the widget boxes.
    // agigl has never had this problem because it renders into an FBO at the pipeline size and
    // blits it to blit_x_/blit_y_/blit_width_/blit_height_; sdlswpipe scales its blit the same way.
    // This backend called InitScaling() and then ignored the answer.
    //
    // It is applied as a coordinate transform rather than agigl's render-to-texture-and-blit, and
    // that choice is about RTX Remix. Remix reconstructs the scene from the draw calls it sees, so
    // world geometry has to keep going to the real backbuffer with its real transforms - the warning
    // dx9target.h opens with. A viewport rectangle and a pretransformed vertex position are both
    // things Remix ignores entirely (it skips pretransformed draws outright), so scaling them costs
    // it nothing, while routing the frame through an offscreen target would have hidden gameplay
    // from it at every resolution below the desktop.
    //
    // Identity whenever the blit rect matches the pipeline size, which is the ordinary windowed case
    // and fullscreen at the desktop resolution - see ScreenScaleActive().
    f32 ScreenScaleX() const
    {
        return screen_scale_x_;
    }

    f32 ScreenScaleY() const
    {
        return screen_scale_y_;
    }

    i32 ScreenOffsetX() const
    {
        return blit_x_;
    }

    i32 ScreenOffsetY() const
    {
        return blit_y_;
    }

    // False means the transform is the identity and callers may take their untouched fast path.
    bool ScreenScaleActive() const
    {
        return screen_scale_active_;
    }

    // Maps a rectangle in logical pixels onto the backbuffer. Right/bottom edges are mapped rather
    // than the width scaled, so adjacent rectangles stay adjacent after rounding.
    void MapScreenRect(i32& x, i32& y, i32& w, i32& h) const;

private:
    void InitScreenScale();

    f32 screen_scale_x_ {1.0f};
    f32 screen_scale_y_ {1.0f};
    bool screen_scale_active_ {};

    agiDX9WorldShader world_shader_ {};
    agiDX9FFPerPixel per_pixel_ {};
    agiDX9RenderTarget scene_target_ {};
    bool scene_target_bound_ {};

    Ptr<agiSurfaceDesc> CaptureScreen();

    Ptr<agiDX9Context> dx9_context_;
    Rc<agiDX9Rasterizer> rasterizer_;
    bool d3d_scene_active_ {};

    // Whether this frame's Remix API lights have gone out yet. See SendRemixFrame.
    bool remix_frame_sent_ {};

    void SendRemixFrame();
};

Owner<agiPipeline> dx9CreatePipeline(i32 argc, char** argv);
