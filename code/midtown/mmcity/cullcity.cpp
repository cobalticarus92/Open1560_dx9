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

define_dummy_symbol(mmcity_cullcity);

#include "cullcity.h"

#include "cellrend.h"

#include "agi/dlptmpl.h"
#include "agi/getdlp.h"
#include "agi/pipeline.h"
#include "agi/rsys.h"
#include "agi/texdef.h"
#include "agisw/swrend.h"
#include "agiworld/getmesh.h"
#include "agiworld/meshlight.h"
#include "agiworld/meshrend.h"
#include "agiworld/meshset.h"
#include "agiworld/quality.h"
#include "sky.h"

#include "agiworld/glowlight.h"
#include "agiworld/skyenv.h"
#include "agiworld/texsheet.h"
#include "arts7/cullmgr.h"
#include "arts7/sim.h"
#include "data7/memstat.h"
#include "localize/localize.h"
#include "mmcityinfo/state.h"
#include "mmdyna/bndtmpl.h"
#include "mmphysics/phys.h"
#include "stream/problems.h"
#include "stream/stream.h"

#include "loader.h"

static constexpr i32 MAX_HIT_BANGERS = 80;
static constexpr i32 MAX_PARTICLES = 100;
static constexpr i32 CHICAGO_CELL_RUNWAY = 35;

// clang-format off
t_mmEnvSetup mmEnvSetup[4][4] {
    { // Morning
        {"sky_mc", "refl_mc", "shadmap_morneve",  0.002, 0.01,   0.0, 0x000000, 1.00, 1.00, 0.50},
        {"sky_mf", "refl_mf", "shadmap_day",      0.010, 0.01, 200.0, 0xBEB496, 1.00, 1.00, 0.75},
        {"sky_mr", "refl_mr", "shadmap_rainsnow", 0.000, 0.01, 500.0, 0x000000, 0.00, 0.00, 0.00},
        {"sky_ms", "refl_mr", "shadmap_rainsnow", 0.000, 0.01, 200.0, 0xFFFFFF, 0.00, 0.00, 0.00},
    },
    { // Noon
        {"sky_nc", "refl_nc", "shadmap_day",      0.010, 0.01,   0.0, 0x000000, 0.25, 0.70, 1.00},
        {"sky_nf", "refl_nf", "shadmap_day",      0.010, 0.01, 200.0, 0x82B4C3, 0.50, 0.85, 1.00},
        {"sky_nr", "refl_nr", "shadmap_rainsnow", 0.000, 0.01, 500.0, 0x000000, 0.50, 0.50, 0.50},
        {"sky_ns", "refl_nr", "shadmap_rainsnow", 0.000, 0.01, 200.0, 0xFFFFFF, 0.50, 0.50, 0.50},
    },
    { // Sunset
        {"sky_sc", "refl_sc", "shadmap_morneve",  0.002, 0.01,   0.0, 0x000000, 1.00, 0.75, 0.50},
        {"sky_sf", "refl_sf", "shadmap_day",      0.010, 0.01, 200.0, 0x5A283C, 1.00, 0.80, 0.65},
        {"sky_sr", "refl_sr", "shadmap_rainsnow", 0.000, 0.01, 500.0, 0x000000, 0.60, 0.50, 0.40},
        {"sky_ss", "refl_sr", "shadmap_rainsnow", 0.000, 0.01, 200.0, 0xFFFFFF, 0.60, 0.50, 0.40},
    },
    { // Night/Dark
        {"sky_dc", "refl_dc", "shadmap_nite",     0.000, 0.01,   0.0, 0x000000, 0.0, 0.0, 0.1},
        {"sky_df", "refl_df", "shadmap_nite",     0.000, 0.01, 300.0, 0x141414, 10.0f/255.0f, 10.0f/255.0f, 10.0f/255.0f},
        {"sky_dr", "refl_dr", "shadmap_nite",     0.000, 0.01, 500.0, 0x000000, 0.0, 0.0, 0.0},
        {"sky_ds", "refl_dr", "shadmap_nite",     0.000, 0.01, 200.0, 0xFFFFFF, 0.0, 0.0, 0.0},
    },
};
// clang-format on

#ifdef ARTS_DEV_BUILD
void mmRunwayLight::AddWidgets(Bank* /*arg1*/)
{}
#endif

void ARTS_FASTCALL mmRunwayLight::FromMatrix(const Matrix34& /*arg1*/)
{}

// ?fix_fill1@@YAXXZ
ARTS_IMPORT /*static*/ void fix_fill1();

// ?fix_fill2@@YAXXZ
ARTS_IMPORT /*static*/ void fix_fill2();

// ?fix_sun@@YAXXZ
ARTS_IMPORT /*static*/ void fix_sun();

// ?parseARGB@@YAXAAI@Z
ARTS_IMPORT /*static*/ void parseARGB(u32& arg1);

// ?parseFloat@@YAXAAM@Z
ARTS_IMPORT /*static*/ void parseFloat(f32& arg1);

// ?parseRGB@@YAXAAI@Z
ARTS_IMPORT /*static*/ void parseRGB(u32& arg1);

// ?parseVector3@@YAXAAVVector3@@@Z
ARTS_IMPORT /*static*/ void parseVector3(Vector3& arg1);

// ?ShowRenderStats@@YAXXZ
ARTS_IMPORT /*static*/ void ShowRenderStats();

i32 mmSky::IsFlashing()
{
    return DoFlash;
}

void mmSky::ClearFlash()
{
    DoFlash = 0;
}

void mmCullCity::Cull()
{
    // Publish the city's vehicle sphere map for the hardware-transform path.
    //
    // Both vehicle draw paths read this same texture out of mmCullCity, but only one of them
    // (mmCarModel::Draw, via DrawLitSph) hands it to agiworld. aiVehicleInstance::Draw passes it
    // straight to the closed SphereMap(), which the native path cannot call - so traffic had no way
    // to name its own reflection texture. agiworld deliberately does not depend on mmcity, so it is
    // pushed down here rather than reached up for. See agiworld/meshrend.h.
    agiNativeCitySphereMap = SphereMap;

    // The lightning latch stood here and is unwired along with the rest of Pathway B
    // (agidx9/dx9pipe.cpp, BeginGfx) - agiLightningFlash had no other reader, so it now stays 0.
    //
    // Worth knowing before rebuilding it: mmSky::DoFlash is a single-frame flag raised by
    // mmRainAudio::Update when thunder plays, and mmSky::Draw CLEARS it as it swaps in FlashTex.
    // Sampling it here, during the cull pass ahead of any drawing, is what makes it observable at
    // all - read it later and it is always zero, because the sky is drawn first. It was then
    // decayed (~0.8/frame) rather than held for one frame, because a strike is not instantaneous
    // and the falloff is what reads as a flash rather than as a dropped frame:
    //
    //     if (mmSky::IsFlashing())
    //         agiLightningFlash = 1.0f;
    //     else
    //         agiLightningFlash *= 0.80f;
    //
    //     if (agiLightningFlash < 0.01f)
    //         agiLightningFlash = 0.0f;

    // Thunder, counted for the renderer (agiworld/skyenv.h) - the RTX Remix Plus sky fires a lightning
    // strike on each one, so the flash lands with the game's own thunder rather than at random.
    //
    // Sampled here for the reason given above: this runs before any drawing, so the flag raised by
    // mmRainAudio::Update this frame is still set. With a textured sky, mmSky::Draw then clears it as
    // it swaps in the flash texture, exactly as before. Without one - the game's own "Textured Sky"
    // option off, which is how the Remix Plus sky hides the dome - nothing draws the sky, nothing
    // clears the flag, and one clap would count again every frame. So it is cleared here instead.
    if (mmSky::IsFlashing())
    {
        ++agiSkyEnv.ThunderCount;

        if (!agiRQ.TexturedSky)
            mmSky::ClearFlash();
    }

    if (FogEnd == 0.0f || agiCurState.GetDrawMode() == agiDrawDepth)
    {
        agiCurState.SetFogMode(agiFogMode::None);
        agiMeshSet::SetFog(0.0, 0);
    }
    else
    {
        agiCurState.SetFogMode(UsePixelFog ? agiFogMode::Pixel : agiFogMode::Vertex);
        agiCurState.SetFogColor(SkyColor | swIsInterlaced);

        f32 fog_end = FogEnd;

        if (!agiCurState.GetSoftwareRendering())
            fog_end = std::min(fog_end, agiRQ.FarClip);

        if (UsePixelFog)
        {
            agiMeshSet::SetFog(0.0f, 0);
            agiCurState.SetFogStart(1.0f);
            agiCurState.SetFogEnd(fog_end);
        }
        else if (UseFogEnd2)
        {
            agiMeshSet::SetFog(FogEnd2, UseFogEnd2);
        }
        else
        {
            agiMeshSet::SetFog(fog_end, 0);
        }
    }
}

static agiMeshCardVertex RainMeshCard[4] {
    {-0.1f, 2.0f, 0.4f, 1.0f},
    {-0.1f, -2.0f, 0.4f, 0.0f},
    {0.1f, -2.0f, 0.6f, 0.0f},
    {0.1f, 2.0f, 0.6f, 1.0f},
};

void mmCullCity::Init(char* name, asCamera* camera)
{
    // -nocull. Here rather than earlier because AutoDetect (mmui/graphics.cpp) writes agiRQ.FarClip
    // when the renderer is chosen, so anything set before this point is overwritten.
    mmApplyNoCullDistances();

    IsSnowing = MMSTATE.Weather == mmWeather::Snow;
    CityName = name;

    TEXSHEET.SetUseAlternate((MMSTATE.TimeOfDay == mmTimeOfDay::Sunset) || (MMSTATE.TimeOfDay == mmTimeOfDay::Night));

    switch (MMSTATE.Weather)
    {
        case mmWeather::Rain: TextureSuffix = "_fall"_xconst; break;
        case mmWeather::Snow: TextureSuffix = "_win"_xconst; break;
        default: TextureSuffix = nullptr; break;
    }

    StaticLog = as_raw arts_fopen(arts_formatf<64>("%s_static.csv", name), "w");

    Loader()->BeginTask(LOC_STRING(MM_IDS_LOADING_CITY_LAYOUT));

    DLPTemplate* city_dlp = nullptr;
    DLPTemplate* lm_dlp = nullptr;

    if (DevelopmentMode)
    {
        city_dlp = GetDLPTemplate(arts_formatf<128>("%scity", name));
        lm_dlp = GetDLPTemplate(arts_formatf<128>("%slm", name));
    }

    ARTS_MEM_STAT("mmCullCity::Init");

    InitProblems();

    Camera = camera;
    AddChild(&BangerDataManager);
    AddChild(&BangerActiveManager);
    AddChild(&BangerManager);
    AddChild(&field_34AF0);
    AddChild(&PHYS);
    AddChild(&RenderWeb);
    AddChild(&Particles);

    Particles.Init(
        MAX_PARTICLES, 4, 4, 4, (MMSTATE.Weather == mmWeather::Rain) ? RainMeshCard : agiMeshSet::DefaultQuad);

    SnowBirthRule.SetName("SnowRule");
    AddChild(&SnowBirthRule);
    SnowBirthRule.Load();

    RainBirthRule.SetName("RainRule");
    AddChild(&RainBirthRule);
    RainBirthRule.Load();

    BirthRule = nullptr;

    Sky.Init("mmsky"_xconst);
    Loader()->EndTask();

    {
        ARTS_MEM_STAT("mmCullCity.WEB");
        RenderWeb.Load(name, 1);
    }

    HitIdBound = RenderWeb.HitIdBound;
    BuildingChain.Init(RenderWeb.MaxCells);
    ObjectsChain.Init(RenderWeb.MaxCells);
    ShadowChain.Init(RenderWeb.MaxCells);

    InitObjectDetail();

    StartOfBangers = static_cast<mmInstance*>(mmInstanceHeap.GetHead());

    LoadBangers(name);

    switch (MMSTATE.GameMode)
    {
        case mmGameMode::Cruise: LoadBangers(arts_formatf<64>("%s_roam", name)); break;
        case mmGameMode::Checkpoint: LoadBangers(arts_formatf<64>("%s_r%d", name, MMSTATE.EventId)); break;
        case mmGameMode::CnR: LoadBangers(arts_formatf<64>("%s_cops", name)); break;
        case mmGameMode::Circuit: LoadBangers(arts_formatf<64>("%s_c%d", name, MMSTATE.EventId)); break;
        case mmGameMode::Blitz: LoadBangers(arts_formatf<64>("%s_b%d", name, MMSTATE.EventId)); break;
    }

    EndOfBangers = new mmYInstance();

    LoadFacades(name);

    EnableSphereCull = CHICAGO;

    if (CHICAGO)
    {
        Vector3 pos_1 = {995.782, 0.173, 1188.804};
        Vector3 pos_2 = {995.782, 0.173, 742.411};
        Vector3 pos_3 = {1015.383, 0.173, 1188.805};
        Vector3 pos_4 = {1015.383, 0.173, 742.411};

        BuildingChain.Parent(new mmRunwayLight("fxltglow"_xconst, pos_1, pos_2), CHICAGO_CELL_RUNWAY);
        BuildingChain.Parent(new mmRunwayLight("fxltglow"_xconst, pos_3, pos_4), CHICAGO_CELL_RUNWAY);
    }

    InitTimeOfDayAndWeather();

    // Publish this race's time of day, weather and the game's fog distances for the renderer - see
    // agiworld/skyenv.h. After InitTimeOfDayAndWeather, so it describes the city exactly as set up.
    {
        const i32 time_of_day = std::clamp(static_cast<i32>(MMSTATE.TimeOfDay), 0, 3);

        agiSkyEnv.TimeOfDay = time_of_day;
        agiSkyEnv.Weather = std::clamp(static_cast<i32>(MMSTATE.Weather), 0, 3);

        for (i32 weather = 0; weather < 4; ++weather)
            agiSkyEnv.FogEnd[weather] = mmEnvSetup[time_of_day][weather].FogEnd;
    }

    if (IsSnowing)
        InitSnowTextures();

    BangerMgr()->Init(MAX_HIT_BANGERS);

    if (city_dlp && city_dlp->Release())
        Errorf("Someone is still holding a ref on the city's template");

    if (lm_dlp)
        lm_dlp->Release();

    CullMgr()->AddPage(ShowRenderStats);

    if (StaticLog)
    {
        delete StaticLog;
    }
}

void mmCullCity::ReparentObject(mmInstance* inst)
{
    if (asPortalCell* cell = RenderWeb.GetStartCell(inst->GetPos(), 0, nullptr))
    {
        if (inst->ChainId == -1)
        {
            ObjectsChain.Parent(inst, cell->CellIndex);
        }
        else if (i16 cell_id = cell->CellIndex; cell_id != inst->ChainId)
        {
            ObjectsChain.Reparent(inst, cell_id);
        }
    }
}

void mmCullCity::Reset()
{
    asNode::Reset();

    // Directly iterating over the instance heep? EEK!
    for (mmInstance* inst = StartOfBangers; inst != EndOfBangers;
        inst = reinterpret_cast<mmInstance*>(reinterpret_cast<char*>(inst) + inst->SizeOf()))
    {
        if (inst->TestFlags(INST_FLAG_40) && !inst->TestFlags(INST_FLAG_UNHIT_BANGER) &&
            inst->SizeOf() == sizeof(mmUnhitBangerInstance))
        {
            inst->Reset();
        }
    }

    if (IsSnowing)
    {
        for (i32 i = 0; i < SnowTextureCount; ++i)
        {
            if (agiTexDef* tex = SnowTexturesDst[i])
            {
                tex->EndGfx();
                tex->SafeBeginGfx();
            }
        }

        if (MMSTATE.TimeOfDay == mmTimeOfDay::Night)
        {
            for (i32 i = 0; i < 10000; ++i)
                UpdateSnowTextures();
        }

        SnowFrictionStartTime = Sim()->GetElapsed();
    }

    Pipe()->Defragment();
}

static mem::cmd_param PARAM_conelighter {"conelighter", "Use agiConeLighter"};

void fix_lighting()
{
    if (PARAM_conelighter)
    {
        mmInstance::DynamicLighter = agiConeLighter;
        mmInstance::StaticLighter = agiConeLighter;
        return;
    }

    switch (agiRQ.LightQuality)
    {
        case AGI_QUALITY_LOW:
            mmInstance::StaticLighter = nullptr;
            mmInstance::DynamicLighter = nullptr;
            break;
        case AGI_QUALITY_MEDIUM:
            mmInstance::DynamicLighter = agiMeshLighterQuarter;
            mmInstance::StaticLighter = agiMeshLighterQuarter;
            break;
        case AGI_QUALITY_HIGH:
            mmInstance::DynamicLighter = agiMeshLighterQuarter;
            mmInstance::StaticLighter = agiMeshLighterTriple;
            break;
        case AGI_QUALITY_VERY_HIGH:
            mmInstance::DynamicLighter = agiMeshLighterTriple;
            mmInstance::StaticLighter = agiMeshLighterTriple;
            break;

        default: Quitf("agiRQ.LightQuality = %d", agiRQ.LightQuality);
    }

    if (agiCurState.GetSoftwareRendering())
        mmInstance::StaticLighter = nullptr;
}