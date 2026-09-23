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

#include "dx9context.h"

#include "dx9rsys.h"
#include "dx9target.h"

#include "dx9_windows.h"

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cctype>
#include <cstring>

define_dummy_symbol(agidx9_dx9context);

void agiDX9Context::CheckErrors(const char* what, long hr)
{
    if (FAILED(hr))
    {
        Errorf("Direct3D9: %s failed, code %x", what, static_cast<u32>(hr));
    }
}

static HWND GetWindowHWND(SDL_Window* window)
{
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

static mem::cmd_param PARAM_d3d9_dll {"d3d9dll", "DLL to take Direct3DCreate9 from"};

using PFN_Direct3DCreate9 = IDirect3D9*(WINAPI*) (UINT sdk_version);

static bool g_remix_bridge_active = false;
static HMODULE g_d3d9_module = nullptr;

bool agiDX9RemixBridgeActive()
{
    return g_remix_bridge_active;
}

void* agiDX9D3D9Module()
{
    return g_d3d9_module;
}

// Whether the module a name resolved to looks like an RTX Remix runtime.
//
// This used to test the *requested* name for "remix" and nothing else, which meant it could
// essentially never fire in a real setup. Remix's normal install IS a drop-in called `d3d9.dll`,
// and chaining proxies (the camera proxy, for one) are also called `d3d9.dll` and load the Remix
// runtime themselves behind the scenes. Both resolve here as the plain name and tested negative.
//
// So test the loaded module instead: `remixapi_InitializeLibrary` is exported by the Remix runtime
// and by nothing else, and the resolved path catches the case where the runtime sits under a name
// of its own. Nothing depends on getting this right any more - see the projection note in
// agiDX9Rasterizer::RestoreStateAfterWorldDraw - it is reported so the log can be believed.
static bool ContainsRemix(const char* text)
{
    char lower[MAX_PATH];

    const size_t len = std::min(std::strlen(text), sizeof(lower) - 1);

    for (size_t i = 0; i < len; ++i)
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));

    lower[len] = '\0';

    return std::strstr(lower, "remix") != nullptr;
}

// Fills out_path with wherever `module` actually came from, which is worth logging on its own:
// "d3d9.dll" resolving to C:\Windows\system32\d3d9.dll says at a glance that no wrapper is loaded,
// and that is exactly the question being asked when path tracing has not turned on.
static bool IsRemixBridgeModule(HMODULE module, const char* name, char* out_path, u32 out_len)
{
    out_path[0] = '\0';

    if (GetModuleFileNameA(module, out_path, out_len) == 0)
        out_path[0] = '\0';

    if (GetProcAddress(module, "remixapi_InitializeLibrary") != nullptr)
        return true;

    return ContainsRemix(name) || ContainsRemix(out_path);
}

// RTX Remix ships its D3D9 entry point as a drop-in replacement *named* `d3d9.dll`, which is how it
// gets loaded at all: the application directory precedes the system directory in the search order.
// That works, but it is a blunt instrument - the name is what does the interception, so the shadow
// applies to every module in the process that resolves d3d9.dll, for the whole run, and the only way
// to turn Remix off is to rename or delete the file.
//
// Resolving the entry point ourselves removes the need for that shadowing entirely: the runtime can
// sit under an unambiguous name, and picking it is a decision this function makes rather than a side
// effect of the loader's search order. Requires that nothing else in the build creates a link-time
// import on d3d9.dll - hence no `links { "d3d9" }` in agidx9/premake5.lua - because a static import
// is bound at process load, before any of this runs. Direct3DCreate9 is the only d3d9 *export* the
// DX9 backend needs; everything else is COM vtable calls through the returned interfaces, which cost
// no imports. This mirrors how dx9shader.cpp reaches D3DCompile, and for the same reason.
//
// Order: -d3d9dll <name> if given, then Remix under its explicit name, then the ordinary d3d9.dll -
// which still finds a drop-in copy in the game folder, so existing setups keep working untouched.
// Created once and then kept alive for the rest of the process, which is a Remix requirement rather
// than an optimization. The engine does not reset the device to change resolution - it destroys the
// entire graphics pipeline and builds a new one, so going from the 640x480 menu into a race runs
// ~agiDX9Context (releasing the IDirect3D9) and then immediately asks for another. Releasing the
// last reference to the D3D9 *module* across that gap is fatal under the Remix bridge: the server
// logs "D3D9 Module destroyed", tears the runtime down, and faults on the next Direct3DCreate9.
//
// One permanent reference held here keeps the module alive across the rebuild. Callers still get a
// reference of their own to release in the usual way, so ~agiDX9Context stays unchanged and the
// count simply never reaches zero.
static IDirect3D9* CreateD3D9()
{
    static IDirect3D9* const cached = []() -> IDirect3D9* {
        const char* candidates[] {PARAM_d3d9_dll.value(), "d3d9_remix.dll", "d3d9.dll"};

        for (const char* name : candidates)
        {
            if (name == nullptr)
                continue;

            HMODULE module = LoadLibraryA(name);

            if (module == nullptr)
                continue;

            auto create = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(module, "Direct3DCreate9"));

            if (create == nullptr)
            {
                Warningf("D3D9: '%s' exports no Direct3DCreate9, skipping", name);

                continue;
            }

            if (IDirect3D9* d3d = create(D3D_SDK_VERSION))
            {
                char path[MAX_PATH];

                g_remix_bridge_active = IsRemixBridgeModule(module, name, path, sizeof(path));
                g_d3d9_module = module;

                Displayf("D3D9: Direct3DCreate9 from '%s' -> '%s'%s", name, (path[0] != '\0') ? path : "<unknown>",
                    g_remix_bridge_active ? " [RTX Remix runtime detected]" : "");

                return d3d;
            }

            Warningf("D3D9: Direct3DCreate9 from '%s' returned null, skipping", name);
        }

        return nullptr;
    }();

    if (cached)
        cached->AddRef();

    return cached;
}

agiDX9Context::agiDX9Context(
    SDL_Window* window, u32 width, u32 height, bool windowed, bool vsync, IDirect3DDevice9* adopt)
    : window_(GetWindowHWND(window))
    , width_(width)
    , height_(height)
    , windowed_(windowed)
    , vsync_(vsync)
{
    d3d_ = CreateD3D9();

    if (!d3d_)
        Quitf("Direct3DCreate9 failed (tried d3d9_remix.dll, then d3d9.dll)");

    if (adopt == nullptr)
    {
        CreateDevice();

        return;
    }

    QueryCaps();

    device_ = adopt;

    if (ResetDevice())
        return;

    // Reset() is expected to succeed: it only fails while a D3DPOOL_DEFAULT resource is still alive,
    // and this backend allocates none - every texture is D3DPOOL_MANAGED (chosen partly to survive a
    // reset, see dx9shader.h), the only offscreen surface is SYSTEMMEM and transient, and geometry
    // goes out through DrawPrimitiveUP rather than device-owned buffers.
    //
    // Should it fail anyway, drop the adopted device and build a fresh one so the game keeps running
    // instead of drawing through a dead device. Say so plainly: this is the destroy/recreate path
    // that upsets the Remix bridge, and a silent fallback would look exactly like the bug it works
    // around.
    Warningf("D3D9: Reset of the adopted device failed, recreating it (Remix: expect a bridge crash)");

    ReleaseDevice();
    CreateDevice();
}

IDirect3DDevice9* agiDX9Context::DetachDevice()
{
    IDirect3DDevice9* device = device_;
    device_ = nullptr;

    return device;
}

agiDX9Context::~agiDX9Context()
{
    ReleaseDevice();

    if (d3d_)
    {
        d3d_->Release();
        d3d_ = nullptr;
    }
}

static void FillPresentParams(
    D3DPRESENT_PARAMETERS& pp, HWND window, u32 width, u32 height, bool windowed, bool vsync, D3DFORMAT depth_format)
{
    std::memset(&pp, 0, sizeof(pp));

    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = windowed ? D3DFMT_UNKNOWN : D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = windowed;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = depth_format;
    pp.PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

// The engine's shared CPU pretransform pipeline (agiMeshSet::Transform/ToScreen) writes a
// standard non-linear perspective depth into agiScreenVtx::z, submitted here as-is via
// D3DFVF_XYZRHW. A 24-bit *fixed-point integer* depth buffer (the classic D3DFMT_D24S8 default)
// quantizes that non-linear value into a fixed set of integer steps that are extremely dense near
// the camera and extremely sparse at the small-to-moderate distances where most actual gameplay
// happens (cars a few meters away, wheels sitting right at the road surface) - exactly the
// distances where this renderer was losing depth fights (missing tires, general z-fighting while
// driving). A 32-bit *floating-point* depth format doesn't suffer that fixed quantization: floats
// naturally concentrate precision wherever the stored value itself is small, so relative
// precision stays far higher at these distances. This has no effect on the wider "which path
// computed this vertex" story (world-space hardware-transformed geometry vs. the CPU-pretransformed
// path both still write into the exact same physical buffer either way) - it just gives every
// draw call, from every path, meaningfully more usable precision to work with.
static D3DFORMAT PickDepthFormat(IDirect3D9* d3d, D3DFORMAT adapter_format)
{
    // NOTE: D3DFMT_D32F_LOCKABLE was tried here previously for better Z precision distribution,
    // but *lockable* depth-stencil formats force the driver to keep the buffer in a CPU-readable
    // layout, which disables hierarchical-Z/early-Z depth-compression fast paths on most GPUs -
    // a well-known D3D9 pitfall. That caused a severe, sustained FPS regression once real scene
    // complexity was on screen (not obvious from short/simple test scenes). Never put a *_LOCKABLE
    // depth format in this list.
    static const D3DFORMAT kCandidates[] = {
        D3DFMT_D24X8, // 24-bit fixed, no wasted/unused stencil bits (we don't use stencil), fast
        D3DFMT_D24S8, // Safe fallback - what this renderer always used before
    };

    for (D3DFORMAT format : kCandidates)
    {
        if (SUCCEEDED(d3d->CheckDeviceFormat(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, adapter_format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, format)) &&
            SUCCEEDED(d3d->CheckDepthStencilMatch(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, adapter_format, adapter_format, format)))
        {
            return format;
        }
    }

    return D3DFMT_D24S8;
}

// Adapter queries only - no device is touched, so this is equally valid before creating one and
// when adopting an existing one.
void agiDX9Context::QueryCaps()
{
    D3DCAPS9 caps {};
    d3d_->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);

    hardware_tl_ = (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;

    max_texture_width_ = caps.MaxTextureWidth;
    max_texture_height_ = caps.MaxTextureHeight;
    max_anisotropy_ = caps.MaxAnisotropy;
    max_simultaneous_textures_ = caps.MaxSimultaneousTextures;
    max_texture_blend_stages_ = caps.MaxTextureBlendStages;
    supports_dot3_ = (caps.TextureOpCaps & D3DTEXOPCAPS_DOTPRODUCT3) != 0;

    // Indexed vertex blending - the fixed-function matrix palette pedestrians are skinned with.
    //
    // MaxVertexBlendMatrixIndex is the largest index a vertex may name, so the palette holds one
    // more than it, and zero means the driver does not support indexed blending at all. Both are
    // reported per adapter and neither can change while a device exists, so this is the right
    // place to read them.
    max_vertex_blend_matrices_ = caps.MaxVertexBlendMatrices;
    max_vertex_blend_matrix_index_ = caps.MaxVertexBlendMatrixIndex;

    D3DDISPLAYMODE display_mode {};
    d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display_mode);

    depth_format_ = static_cast<u32>(PickDepthFormat(d3d_, display_mode.Format));
}

void agiDX9Context::CreateDevice()
{
    QueryCaps();

    D3DPRESENT_PARAMETERS pp;
    FillPresentParams(
        pp, static_cast<HWND>(window_), width_, height_, windowed_, vsync_, static_cast<D3DFORMAT>(depth_format_));

    u32 vertex_processing = hardware_tl_ ? D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING;

    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, static_cast<HWND>(window_),
        vertex_processing | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED, &pp, &device_);

    if (FAILED(hr) && vertex_processing == D3DCREATE_HARDWARE_VERTEXPROCESSING)
    {
        // Some very old/virtual GPUs advertise HWTL but fail to create a device with it
        hardware_tl_ = false;

        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, static_cast<HWND>(window_),
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED, &pp, &device_);
    }

    if (FAILED(hr))
        Quitf("IDirect3D9::CreateDevice failed, code %x", static_cast<u32>(hr));

    // A brand new device holds nothing any mirror in agidx9 remembers. See agiDX9OnDeviceReset().
    agiDX9OnDeviceReset(device_);
}

void agiDX9Context::ReleaseDevice()
{
    if (device_)
    {
        device_->Release();
        device_ = nullptr;
    }
}

bool agiDX9Context::ResetDevice()
{
    D3DPRESENT_PARAMETERS pp;
    FillPresentParams(
        pp, static_cast<HWND>(window_), width_, height_, windowed_, vsync_, static_cast<D3DFORMAT>(depth_format_));

    // Release every D3DPOOL_DEFAULT allocation first. D3D9 fails Reset with D3DERR_INVALIDCALL
    // while even one is alive, and here that failure is not something to shrug at: Reset is on the
    // path the menu <-> gameplay resolution change takes when a new context adopts the parked
    // device, and the fallback when it fails is to destroy the device and build a new one - exactly
    // what breaks the RTX Remix bridge the parked device exists to protect.
    //
    // Everything else this backend allocates is D3DPOOL_MANAGED and survives a reset untouched, so
    // with no render targets in play this list is empty and costs nothing. See
    // agiDX9DefaultPoolResource in dx9target.h.
    agiDX9ReleaseDefaultPoolResources();

    HRESULT hr = device_->Reset(&pp);

    if (FAILED(hr))
    {
        if (hr == D3DERR_INVALIDCALL)
        {
            Errorf("D3D9: Reset returned D3DERR_INVALIDCALL - a D3DPOOL_DEFAULT resource outlived "
                   "agiDX9ReleaseDefaultPoolResources() (%s)",
                agiDX9HasDefaultPoolResources() ? "registered resources remain"
                                                : "no registered resources - unowned allocation");
        }
        else
        {
            CheckErrors("Reset", hr);
        }

        return false;
    }

    if (!agiDX9RestoreDefaultPoolResources(device_))
        Warningf("D3D9: some default-pool resources could not be recreated after Reset");

    // Reset() returned every render state, sampler, transform, light and material to its D3D9
    // default. Say so, before anything draws through a cache that still describes the device as it
    // was - a mirror that has not heard about a reset SUPPRESSES the writes that would repair it.
    // See agiDX9OnDeviceReset().
    agiDX9OnDeviceReset(device_);

    return true;
}

bool agiDX9Context::BeginFrame()
{
    if (device_lost_)
    {
        HRESULT hr = device_->TestCooperativeLevel();

        if (hr == D3DERR_DEVICELOST)
            return false;

        if (hr == D3DERR_DEVICENOTRESET)
        {
            if (!ResetDevice())
                return false;
        }

        device_lost_ = false;
    }

    return true;
}

bool agiDX9Context::Present()
{
    HRESULT hr = device_->Present(nullptr, nullptr, nullptr, nullptr);

    if (hr == D3DERR_DEVICELOST)
    {
        device_lost_ = true;
        return false;
    }

    return true;
}

void agiDX9Context::Resize(u32 width, u32 height, bool windowed, bool vsync)
{
    width_ = width;
    height_ = height;
    windowed_ = windowed;
    vsync_ = vsync;

    if (ResetDevice())
        return;

    // Reset() is expected to succeed here: ResetDevice() releases every registered D3DPOOL_DEFAULT
    // resource before calling it, and everything else this backend allocates is D3DPOOL_MANAGED
    // (which survives a reset by design, see the note in dx9shader.h), the only offscreen surface
    // is SYSTEMMEM and transient, and geometry goes out through DrawPrimitiveUP rather than
    // device-owned buffers.
    //
    // If it fails anyway, fall back to building a new device so the game keeps running rather than
    // rendering through a dead one. Note this is the path that upsets the Remix bridge, so say so
    // plainly - a silent fallback would look exactly like the bug it is working around.
    Warningf("D3D9: device Reset failed, recreating the device (Remix users: expect a bridge crash)");

    ReleaseDevice();
    CreateDevice();

    // The failed ResetDevice() already released them; the new device needs them back. Without this
    // every render target would stay dead for the rest of the run after one failed reset.
    if (!agiDX9RestoreDefaultPoolResources(device_))
        Warningf("D3D9: some default-pool resources could not be recreated on the new device");
}
