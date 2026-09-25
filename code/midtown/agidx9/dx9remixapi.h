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

// Not part of the original engine/binary.
//
// The Remix API's own types, for the agidx9 files that talk to the bridge directly. Internal to
// agidx9: everything else goes through dx9remix.h, which exposes no Remix types.

// remix_c.h is third-party (MIT, NVIDIA and the Remix Plus contributors), vendored unmodified from
// Remix Plus (RemixProjGroup/dxvk-remix 9aab34bd, API 0.1000.0) - see dx9remix.h for why that copy.
// It lives in vendor/remix with the other third-party code, outside code/, which keeps it out of the
// project's clang-format check: it is someone else's file, and stays byte-identical to theirs.
// It is kept out of this project's warning level rather than edited: its error-code enum carries
// HRESULT-style values above INT_MAX, which a strict build flags.
//
// REMIX_ALLOW_X86: the header refuses 32-bit targets because the ray tracing runtime cannot run in
// one. That is true and beside the point - we only use its types, and the bridge client that
// implements them IS 32-bit. The Remix Plus API reference documents exactly this use.
//
// dx9_windows.h first, always: remix_c.h includes <windows.h> itself, and whichever inclusion comes
// first decides whether min/max are macros. Without NOMINMAX (set by core/minwin.h, which
// dx9_windows.h pulls in) they are, and every std::min/std::max/std::clamp after this header breaks.
//
// REMIX_WINAPI_NO_LIBRARY_LOADER: skips the header's inline DLL loader. We never load the runtime
// ourselves; the bridge client is already the process's D3D9 module.
#include "dx9_windows.h"

#pragma warning(push, 0)
#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Weverything"
#endif
#ifndef REMIX_ALLOW_X86
#    define REMIX_ALLOW_X86
#endif
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include "remix/remix_c.h"
#ifdef __clang__
#    pragma clang diagnostic pop
#endif
#pragma warning(pop)

// The scene half of the interface - materials, meshes and instances - for the modules that add
// geometry of their own (dx9remixwet.cpp). Taken from the same table as the light functions; see
// IdentifyBridge in dx9remix.cpp.
struct agiDX9RemixSceneApi
{
    PFN_remixapi_CreateMaterial CreateMaterial;
    PFN_remixapi_DestroyMaterial DestroyMaterial;
    PFN_remixapi_CreateMesh CreateMesh;
    PFN_remixapi_DestroyMesh DestroyMesh;
    PFN_remixapi_DrawInstance DrawInstance;
};

// Null unless the API is active and the bridge filled all five.
const agiDX9RemixSceneApi* agiDX9RemixApiScene();
