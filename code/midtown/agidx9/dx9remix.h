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
// The RTX Remix API channel: game data that never reaches Remix through D3D9 calls, handed to the
// runtime directly. See docs/remix_api_plan.md for the design and the phases still to come.
//
// This game is 32-bit, and the Remix runtime is 64-bit only, so nothing here talks to the runtime
// itself. It talks to the bridge client - the 32-bit d3d9.dll - which exports its own
// remixapi_InitializeLibrary and forwards each call to the 64-bit server over the same channel as
// the D3D9 commands. Everything below is shaped by what the bridges support, not by what remix_c.h
// declares. Two bridges are supported, NVIDIA's bridge-remix and Remix Plus's (the `bridge` folder
// of RemixProjGroup/dxvk-remix), and they share these rules:
//
//   - A bridge refuses to initialise unless bridge.conf has `exposeRemixApi = True`.
//   - It ignores the version we pass and copies out its interface table laid out by ITS header.
//     Three layouts are in circulation, so the table is identified before anything in it is called
//     - see IdentifyBridge in the .cpp. vendor/remix/remix/remix_c.h is Remix Plus's current header
//     (0.1000.0).
//   - Only lights, materials, meshes, instances and SetConfigVariable are forwarded. Remix Plus also
//     forwards SetGameValue/GetGameValue; its UpdateLightDefinition and AutoInstancePersistentLights
//     are bridge stubs that fail, so a 32-bit game cannot use them yet.
//   - Every CreateLight mints a new bridge handle, and only DestroyLight frees it. Recreating a
//     light in place (same hash, no destroy) would leak one server-side map entry per call, so an
//     update is a destroy and a create - under a new hash, because Remix Plus defers the destroy.

// Initialises the API once per process, the first time it is called with the Remix bridge
// detected and -remixapi on. Must run after the D3D9 device exists: the bridge binds API calls to
// the most recently created device. Safe to call from every BeginGfx.
void agiDX9RemixApiInit();

// True once the bridge has handed back a working interface.
bool agiDX9RemixApiActive();

// Sends this frame's lights. Once per frame, before Present - Remix clears its list of drawn API
// lights every frame, so a light not drawn again simply goes dark.
void agiDX9RemixApiSubmitFrame();

// Destroys every light this module has created. Called when the pipeline is torn down: the city the
// lights describe is about to be freed, and the next pipeline starts from an empty registry.
void agiDX9RemixApiReleaseAll();

// Applies a Remix option (an rtx.conf key) at runtime. False if the API is not active, or if the
// call could not be sent. The runtime ignores keys it does not know.
bool agiDX9RemixApiSetConfig(const char* key, const char* value);

// Remix Plus's game-value channel (SetGameValue). Its sky and weather system is driven through it -
// see dx9remixsky.h. HasGameValues is false on NVIDIA's bridge, which does not forward it. Each call
// is a blocking round trip to the bridge server, so this is for occasional state, not per frame.
bool agiDX9RemixApiHasGameValues();
bool agiDX9RemixApiSetGameValue(const char* key, const char* value);

// One census line, and resets the windowed counters. Called from the pipeline's 120-frame census.
void agiDX9RemixApiLogStats(u32 frame);
