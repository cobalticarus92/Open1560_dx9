arts_component "agidx9"
    files {
        "dx9pipe.cpp",
        "dx9pipe.h",
        "dx9context.cpp",
        "dx9context.h",
        "dx9_windows.h",
        "dx9texdef.cpp",
        "dx9texdef.h",
        "dx9view.cpp",
        "dx9view.h",
        "dx9bitmap.cpp",
        "dx9bitmap.h",
        "dx9rsys.cpp",
        "dx9rsys.h",
        "dx9config.cpp",
        "dx9config.h",
        "dx9ffshade.cpp",
        "dx9ffshade.h",
        "dx9shader.cpp",
        "dx9shader.h",
        "dx9target.cpp",
        "dx9target.h",
        "dx9probe.cpp",
        "dx9probe.h",
        "dx9remix.cpp",
        "dx9remix.h",
        "dx9remixsky.cpp",
        "dx9remixsky.h",
        "remix_c.h",
    }

    -- Deliberately no `links { "d3d9" }`. A link-time import binds d3d9.dll at process load, which
    -- is before any code runs and so before anything can choose *which* d3d9 to use - and, with a
    -- Remix drop-in present, the application directory wins that lookup for the whole process.
    -- Direct3DCreate9 is resolved by name at runtime instead; see CreateD3D9() in dx9context.cpp.
    includeSDL3()
