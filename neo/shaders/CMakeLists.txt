include(../compileshaders.cmake)

file(GLOB globalshaders "BRDF.inc.hlsl" "global_inc.hlsl" "vulkan.hlsli" "renderParmSet0.inc.hlsl" "renderParmSet1.inc.hlsl" "renderParmSet2.inc.hlsl" "renderParmSet3.inc.hlsl" "renderParmSet4.inc.hlsl" "renderParmSet5.inc.hlsl" "renderParmSet6.inc.hlsl" "renderParmSet7.inc.hlsl" "renderParmSet8.inc.hlsl" "renderParmSet9.inc.hlsl" "renderParmSet10.inc.hlsl" "renderParmSet11.inc.hlsl" "renderParmSet12.inc.hlsl" "renderParmSet13.inc.hlsl" "renderParmSet14.inc.hlsl")
file(GLOB builtins "builtin/*.hlsl")
file(GLOB debug "builtin/debug/*hlsl")
file(GLOB fog "builtin/fog/*hlsl")
file(GLOB legacy "builtin/legacy/*hlsl")
file(GLOB lighting "builtin/lighting/*hlsl")
file(GLOB post "builtin/post/*hlsl")
file(GLOB ssao "builtin/SSAO/*hlsl")
file(GLOB ddgi "builtin/ddgi/*hlsl")
file(GLOB reflections "builtin/reflections/*hlsl")
file(GLOB embers "builtin/embers/*hlsl")
file(GLOB ssgi "builtin/SSGI/*hlsl")
file(GLOB video "builtin/video/*hlsl")
file(GLOB vr "builtin/VR/*hlsl")
file(GLOB usershaders "*.hlsl")

list(REMOVE_ITEM usershaders ${globalshaders})

set(shaders ${globalshaders} ${builtins} ${debug} ${fog} ${legacy} ${lighting} ${post} ${ssao} ${embers} ${ssgi} ${video} ${vr} ${usershaders})

source_group("builtin" FILES ${builtins})
source_group("builtin/debug" FILES ${debug})
source_group("builtin/fog" FILES ${fog})
source_group("builtin/legacy" FILES ${legacy})
source_group("builtin/lighting" FILES ${lighting})
source_group("builtin/post" FILES ${post})
source_group("builtin/SSAO" FILES ${ssao})
source_group("builtin/ddgi" FILES ${ddgi})
source_group("builtin/reflections" FILES ${reflections})
source_group("builtin/embers" FILES ${embers})
source_group("builtin/SSGI" FILES ${ssgi})
source_group("builtin/video" FILES ${video})
source_group("builtin/VR" FILES ${vr})
source_group("global" FILES ${globalshaders})

source_group("user" FILES ${usershaders})

# ---------------------------------------------------------------------------
# DEV-ONLY shader permutation subset (fast iteration on softterm.cs.hlsl and its
# includes, e.g. softwedge_coverage.inc.hlsl).
#
# softterm.cs.hlsl declares a 1536-permutation matrix in shaders.cfg (SW_FACE_SAMPLES
# {8,16,32} x SW_SCAN_CHORDS {4,8,16,32} x SW_ADAPT_CHORDS {0,1} x five feature axes).
# Touching an .inc it includes correctly invalidates ONLY those 1536 perms - but each
# is a full DXC -O3 compile, so an edit-build loop is ~15 min even though ShaderMake
# is already multi-threaded.
#
# SHADER_DEV_SUBSET pins the three pure QUALITY/instrument axes to their gate/bench
# defaults (SW_FACE_SAMPLES=16, SW_SCAN_CHORDS=16, SW_ADAPT_CHORDS=0) => 64 perms (24x
# fewer). The five FEATURE axes stay full, so com_softShadowGate / com_softShadowFrameProbe
# and every default sweep they run (surf-cache, grid, scanline, contrib, walk-counters,
# sub-sample) still resolve their permutation and pass init with no missing-shader errors.
#
# OPT-IN, DEV ONLY. A subset build is NOT shippable: r_softShadowSamples 8/32,
# r_softShadowScanChords 4/8/32 and r_softShadowAdaptiveChords 1 will not have a
# compiled permutation (the feature silently stays off). Keep OFF for release builds
# and for correctness testing across the quality axes.
# ---------------------------------------------------------------------------
# OFF=full matrix (shipping, only shippable value). QUALITY=pin the quality axes s16/c16/a0
# (~64 perms; all FEATURE axes still resolve). SINGLE=only the 2 default-bench softterm perms
# (fastest: the .inc-touch loop no longer rebuilds the softterm matrix at all). QUALITY/SINGLE
# stay -O3 so their ms is bench-valid for the DEFAULT cvars; both are DEV ONLY (off-default
# soft-shadow cvars have no compiled perm). (Ported opt-in only from Percy's worktree - NOT the
# permanent shaders.cfg prune, NOT the -O1 hatch: the wedge wiring may request new perm combos,
# and ms must stay -O3.)
set(SHADER_DEV_SUBSET "OFF" CACHE STRING "DEV ONLY softterm perm subset: OFF (full/shippable), QUALITY (~64 perms s16c16a0), SINGLE (2 default-bench perms, fastest). QUALITY/SINGLE are -O3 dev-only.")
set_property(CACHE SHADER_DEV_SUBSET PROPERTY STRINGS OFF QUALITY SINGLE)

set(SHADERS_CFG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg)
if(NOT SHADER_DEV_SUBSET STREQUAL "OFF")
    # Generated cfg lives in the source dir so ShaderMake's source-path resolution
    # (configFile.parent_path() / source) stays correct without extra plumbing.
    set(SHADERS_CFG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.dev_subset.cfg)
    if(SHADER_DEV_SUBSET STREQUAL "SINGLE")
        # drop EVERY softterm line, append only the 2 perms the default bench dispatches
        # (shipped SW_SCANLINE=0 + scanline SW_SCANLINE=1, both s16/c16/a0, caches off).
        file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg _lines)
        set(_subset_cfg "")
        foreach(_l IN LISTS _lines)
            if(NOT _l MATCHES "^builtin/lighting/softterm\\.cs\\.hlsl")
                string(APPEND _subset_cfg "${_l}\n")
            endif()
        endforeach()
        string(APPEND _subset_cfg "builtin/lighting/softterm.cs.hlsl -T cs -D SW_GPU_WALK_COUNTERS=0 -D SW_SURF_CACHE=0 -D SW_SURF_GRID=0 -D SW_SCANLINE=0 -D SW_CONTRIB_CACHE=0 -D SW_FACE_SAMPLES=16 -D SW_SCAN_CHORDS=16 -D SW_ADAPT_CHORDS=0 -D SW_SUBSAMPLE=0\n")
        string(APPEND _subset_cfg "builtin/lighting/softterm.cs.hlsl -T cs -D SW_GPU_WALK_COUNTERS=0 -D SW_SURF_CACHE=0 -D SW_SURF_GRID=0 -D SW_SCANLINE=1 -D SW_CONTRIB_CACHE=0 -D SW_FACE_SAMPLES=16 -D SW_SCAN_CHORDS=16 -D SW_ADAPT_CHORDS=0 -D SW_SUBSAMPLE=0\n")
        message(STATUS "SHADER_DEV_SUBSET=SINGLE: softterm = 2 default-bench perms (-O3). DEV ONLY - off-default soft-shadow cvars have NO shader.")
    else() # QUALITY: pin the three quality axes, keep the five feature axes full (~64 perms)
        file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg _subset_cfg)
        string(REGEX REPLACE "SW_FACE_SAMPLES=\\{[^}]*\\}" "SW_FACE_SAMPLES={16}" _subset_cfg "${_subset_cfg}")
        string(REGEX REPLACE "SW_SCAN_CHORDS=\\{[^}]*\\}"  "SW_SCAN_CHORDS={16}"  _subset_cfg "${_subset_cfg}")
        string(REGEX REPLACE "SW_ADAPT_CHORDS=\\{[^}]*\\}" "SW_ADAPT_CHORDS={0}"  _subset_cfg "${_subset_cfg}")
        message(STATUS "SHADER_DEV_SUBSET=QUALITY: softterm pinned s16/c16/a0 (~64 perms, -O3). DEV ONLY - never ship.")
    endif()
    # Write only when the content changes, so re-running cmake does not bump the cfg
    # mtime and force a needless full rebuild of everything that references it.
    set(_subset_existing "")
    if(EXISTS ${SHADERS_CFG})
        file(READ ${SHADERS_CFG} _subset_existing)
    endif()
    if(NOT _subset_existing STREQUAL _subset_cfg)
        file(WRITE ${SHADERS_CFG} "${_subset_cfg}")
    endif()
endif()

if(STANDALONE)
    compile_shaders_all_platforms(
        TARGET Shaders
        CONFIG ${SHADERS_CFG}
        OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../content/renderprogs2
        SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
        SOURCES ${shaders}
    )
else()
    compile_shaders_all_platforms(
        TARGET Shaders
        CONFIG ${SHADERS_CFG}
        OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../base/renderprogs2 # This path scheme is a bit odd.
        SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
        SOURCES ${shaders}
    )
endif()

# ---------------------------------------------------------------------------
# DDGI ray tracing shaders (inline ray query) require shader model 6.6, so they
# are compiled as a SEPARATE target. The global SPIR-V build is now SM 6.2 (for
# the packed-fp16 soft-shadow loop's explicit float16_t); the legacy 'half'
# types are pinned back to float via -D half=float etc. in compileshaders.cmake
# so their semantics (and global_inc.hlsl under -Werror) are unchanged. The DDGI
# shaders are self-contained (they do not include global_inc.hlsl) so 6.6 is
# safe for them. Output goes to the same renderprogs2/spirv directory the
# engine loads from.
# ---------------------------------------------------------------------------
if(STANDALONE)
    set(DDGI_OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../content/renderprogs2)
else()
    set(DDGI_OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../base/renderprogs2)
endif()

add_custom_target(ShadersRT
    DEPENDS ShaderMake
    SOURCES ${ddgi} ${reflections}
)

if(USE_VULKAN)
    add_custom_command(TARGET ShadersRT PRE_BUILD
                      COMMAND ShaderMake
                               --config=${CMAKE_CURRENT_SOURCE_DIR}/shaders_rt.cfg
                               --out=${DDGI_OUTPUT_BASE}/spirv
                               --platform=SPIRV
                               --binaryBlob
                               --outputExt=.bin
                               --retryCount=20
                               -I ${CMAKE_CURRENT_SOURCE_DIR}
                               -D SPIRV
                               --vulkanVersion=1.2 --shaderModel=6_6 -O3 --WX --matrixRowMajor
                               --tRegShift=0 --sRegShift=128 --bRegShift=256 --uRegShift=384
                               --compiler=${DXC_SPIRV_PATH})
endif()
