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

// Loads Open1560_RemixAPI.ini: what the game sends to RTX Remix through the Remix API - its glow
// lights, and (Remix Plus) the sky and weather.
//
// Most keys are simply existing mem::cmd_params, applied through mem::cmd_param::set() exactly as a
// command-line switch would be. That keeps one source of truth for defaults and means anything
// tunable on the command line is tunable from the file and vice versa. The [Glow.<Kind>] and
// [Glow:<TEXTURE>] sections are the exception: per-kind and per-texture offsets, sizes and colours
// have no switch, and go to the glow tuning table (agiworld/glowtune.h). A kind section's enabled and
// intensity are still that kind's switches.
//
// MUST be called before mem::cmd_param::init(argc, argv). Later assignments overwrite earlier ones,
// so loading first is what gives the command line precedence over the file - the right way round,
// since the file is a persistent preference and the command line is a deliberate one-off override.
//
// Writes a fully commented template on first run if the file is absent, so the available knobs are
// discoverable without reading the source. If the file it replaces, Open1560-Shaders.ini, is present
// at that point, its settings are written into the new file, and the old one is not read again.
void agiDX9LoadRemixConfig();
