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
file(GLOB ssgi "builtin/SSGI/*hlsl")
file(GLOB video "builtin/video/*hlsl")
file(GLOB vr "builtin/VR/*hlsl")
file(GLOB usershaders "*.hlsl")

list(REMOVE_ITEM usershaders ${globalshaders})

set(shaders ${globalshaders} ${builtins} ${debug} ${fog} ${legacy} ${lighting} ${post} ${ssao} ${ssgi} ${video} ${vr} ${usershaders})

source_group("builtin" FILES ${builtins})
source_group("builtin/debug" FILES ${debug})
source_group("builtin/fog" FILES ${fog})
source_group("builtin/legacy" FILES ${legacy})
source_group("builtin/lighting" FILES ${lighting})
source_group("builtin/post" FILES ${post})
source_group("builtin/SSAO" FILES ${ssao})
source_group("builtin/ddgi" FILES ${ddgi})
source_group("builtin/SSGI" FILES ${ssgi})
source_group("builtin/video" FILES ${video})
source_group("builtin/VR" FILES ${vr})
source_group("global" FILES ${globalshaders})

source_group("user" FILES ${usershaders})

if(STANDALONE)
    compile_shaders_all_platforms(
        TARGET Shaders
        CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
        OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../content/renderprogs2
        SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
        SOURCES ${shaders}
    )
else()
    compile_shaders_all_platforms(
        TARGET Shaders
        CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
        OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../base/renderprogs2 # This path scheme is a bit odd.
        SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
        SOURCES ${shaders}
    )
endif()

# ---------------------------------------------------------------------------
# DDGI ray tracing shaders (inline ray query) require shader model 6.6, so they
# are compiled as a SEPARATE target. Bumping the global shader model would enable
# -enable-16bit-types and change fp16 semantics across every shader (and break
# global_inc.hlsl under -Werror). The DDGI shaders are self-contained (they do
# not include global_inc.hlsl) so 6.6 is safe for them. Output goes to the same
# renderprogs2/spirv directory the engine loads from.
# ---------------------------------------------------------------------------
if(STANDALONE)
    set(DDGI_OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../content/renderprogs2)
else()
    set(DDGI_OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../base/renderprogs2)
endif()

add_custom_target(ShadersRT
    DEPENDS ShaderMake
    SOURCES ${ddgi}
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
