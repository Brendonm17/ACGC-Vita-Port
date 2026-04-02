cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
    else()
        message(FATAL_ERROR "VITASDK not set")
    endif()
endif()

project(ac_vita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(VITA_APP_NAME "Animal Crossing")
set(VITA_TITLEID  "ACGC00001")
set(VITA_VERSION  "01.00")

# decomp has pervasive UB — disable all GCC transforms that exploit it
# -Os with UB-safe flags is the only level that doesn't crash
set(COMMON_SAFETY_FLAGS "-fno-strict-aliasing -fwrapv -fno-math-errno -fno-signed-zeros -fno-trapping-math -freciprocal-math -fdata-sections -ffunction-sections")
set(UB_SAFE_FLAGS "-fno-tree-vrp -fno-tree-ccp -fno-delete-null-pointer-checks -fno-aggressive-loop-optimizations -fno-strict-overflow -fno-isolate-erroneous-paths-dereference -fno-isolate-erroneous-paths-attribute")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Os -g1 ${COMMON_SAFETY_FLAGS} ${UB_SAFE_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os -g1 ${COMMON_SAFETY_FLAGS} ${UB_SAFE_FLAGS} -fpermissive")

add_compile_definitions(
    TARGET_VITA
    TARGET_PC
    __vita__
    VERSION=0
    F3DEX_GBI_2
    NDEBUG
    _LANGUAGE_C
    BUGFIXES
    PC_ENHANCEMENTS
    HAVE_GLSL
    SKIP_ERROR_CAPTURE
    NO_TEX_COMBINER
)

add_compile_options(-fno-strict-aliasing -fwrapv -w)

set(DECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

include_directories(
    ${DECOMP_ROOT}/include
    ${DECOMP_ROOT}/src
    ${DECOMP_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    $ENV{VITASDK}/arm-vita-eabi/include
    ${DECOMP_ROOT}/pc/include
    ${DECOMP_ROOT}/pc/lib
)

# game source files (same filtering as PC port)
file(GLOB_RECURSE GAME_C_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/*.c")
file(GLOB_RECURSE GAME_CXX_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/*.cpp")

# exclusion filters (same as PC port)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/AUS/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/gx/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/os/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/vi/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/pad/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/card/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/dvd/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/ai/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/ar/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/dsp/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/exi/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/si/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/db/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/mtx/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/base/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/gba/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/amcstubs/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/OdemuExi2/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/odenotstub/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/TRK_MINNOW_DOLPHIN/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/Runtime\\.PPCEABI\\.H/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*")

file(GLOB_RECURSE DATA_C_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/data/*.c")
list(APPEND GAME_C_SOURCES ${DATA_C_SOURCES})

list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/src/furniture/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/src/data/model/obj_e_boat/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ac_npc_shop_common\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ac_animal_logo_misc\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/m_item_debug\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/actor/npc/.*_anime\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/osreport\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/Famicom/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/MSL_C/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/jaudio_NES/internal/dsp_cardunlock\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/jaudio_NES/internal/dsp_GBAKey\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libc64/malloc\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libc64/sprintf\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libc64/aprintf\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libultra/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ReconfigBATs\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/executor\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ef_effect_lib\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/m_card\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/emu64_utility\\.c$")

list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/AUS/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/dolphin/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/TRK_MINNOW_DOLPHIN/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/Runtime\\.PPCEABI\\.H/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/Famicom/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/emu64_print\\.cpp$")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/jsyswrapper_ext\\.cpp$")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/jsyswrapper_main\\.cpp$")

# split data (static lib, compile once) from code (active development)
set(DATA_SOURCES "")
set(CODE_C_SOURCES "")

foreach(src ${GAME_C_SOURCES})
    if(src MATCHES ".*/src/data/.*")
        list(APPEND DATA_SOURCES ${src})
    else()
        list(APPEND CODE_C_SOURCES ${src})
    endif()
endforeach()

list(LENGTH DATA_SOURCES DATA_COUNT)
list(LENGTH CODE_C_SOURCES CODE_COUNT)
list(LENGTH GAME_CXX_SOURCES CXX_COUNT)
message(STATUS "Data files (static lib, compile once): ${DATA_COUNT}")
message(STATUS "Game code C files: ${CODE_COUNT}")
message(STATUS "Game code C++ files: ${CXX_COUNT}")

if(DATA_SOURCES)
    add_library(game_data STATIC ${DATA_SOURCES})
    target_compile_options(game_data PRIVATE -w)
endif()

set(VITA_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_platform.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_gx_compat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_vtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_gx_cmdbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_banner.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_texpack.c

    ${DECOMP_ROOT}/pc/src/pc_gx.c
    ${DECOMP_ROOT}/pc/src/pc_gx_tev.c
    ${DECOMP_ROOT}/pc/src/pc_gx_texture.c
    ${DECOMP_ROOT}/pc/src/pc_os.c
    ${DECOMP_ROOT}/pc/src/pc_dvd.c
    ${DECOMP_ROOT}/pc/src/pc_card.c
    ${DECOMP_ROOT}/pc/src/pc_vi.c
    ${DECOMP_ROOT}/pc/src/pc_audio.c
    ${DECOMP_ROOT}/pc/src/pc_aram.c
    ${DECOMP_ROOT}/pc/src/pc_misc.c
    ${DECOMP_ROOT}/pc/src/pc_mtx.c
    ${DECOMP_ROOT}/pc/src/pc_m_card.c
    ${DECOMP_ROOT}/pc/src/pc_save_bswap.c
    ${DECOMP_ROOT}/pc/src/pc_settings.c
    ${DECOMP_ROOT}/pc/src/pc_stubs.c
    ${DECOMP_ROOT}/pc/src/pc_stubs_cpp.cpp
    ${DECOMP_ROOT}/pc/src/pc_assets.c
    ${DECOMP_ROOT}/pc/src/pc_disc.c
    ${DECOMP_ROOT}/pc/src/pc_pad.c
    ${DECOMP_ROOT}/pc/src/pc_keybindings.c
    ${DECOMP_ROOT}/src/static/dolphin/pad/Padclamp.c
    ${DECOMP_ROOT}/pc/lib/stb_image.c
)

set_source_files_properties(
    "${DECOMP_ROOT}/src/main.c"
    PROPERTIES COMPILE_DEFINITIONS "main=ac_entry"
)
set_source_files_properties(
    "${DECOMP_ROOT}/src/static/boot.c"
    PROPERTIES COMPILE_DEFINITIONS "main=boot_main"
)

set_source_files_properties(
    "${DECOMP_ROOT}/src/static/libforest/emu64/emu64.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/ja_calc.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/jammain_2.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/game/game64.c"
    PROPERTIES LANGUAGE CXX
)

add_compile_options($<$<COMPILE_LANGUAGE:C>:-Dnullptr=NULL>)

# decomp: -Og is the only level that doesn't crash (pervasive UB)
set(DECOMP_C_FLAGS "-Og;-w")
set(DECOMP_CXX_FLAGS "-Og;-w;-fpermissive")
set_source_files_properties(${CODE_C_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${DECOMP_C_FLAGS}"
)
set_source_files_properties(${GAME_CXX_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${DECOMP_CXX_FLAGS}"
)

# emu64: hottest file, ~92% of frame time — max optimization + LTO
set_source_files_properties(
    "${DECOMP_ROOT}/src/static/libforest/emu64/emu64.c"
    PROPERTIES COMPILE_OPTIONS "-O3;-w;-flto;-funroll-loops;-ffast-math;-ftree-vectorize"
)

# port layer: our code, no UB, safe at -O2
set_source_files_properties(
    ${VITA_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O2;-w"
)
# GX hot path: -O3 + LTO for cross-file inlining into emu64
set_source_files_properties(
    "${DECOMP_ROOT}/pc/src/pc_gx.c"
    "${DECOMP_ROOT}/pc/src/pc_gx_tev.c"
    "${DECOMP_ROOT}/pc/src/pc_gx_texture.c"
    PROPERTIES COMPILE_OPTIONS "-O3;-w;-flto;-ffinite-math-only;-funroll-loops;-ffast-math;-ftree-vectorize"
)
set_source_files_properties(
    "${DECOMP_ROOT}/pc/src/pc_mtx.c"
    PROPERTIES COMPILE_OPTIONS "-O3;-w;-flto;-ffinite-math-only;-funroll-loops;-ffast-math;-ftree-vectorize;-mfpu=neon"
)
set_source_files_properties(
    "${DECOMP_ROOT}/pc/src/pc_os.c"
    "${DECOMP_ROOT}/pc/src/pc_dvd.c"
    "${DECOMP_ROOT}/pc/src/pc_card.c"
    "${DECOMP_ROOT}/pc/src/pc_vi.c"
    "${DECOMP_ROOT}/pc/src/pc_audio.c"
    "${DECOMP_ROOT}/pc/src/pc_aram.c"
    "${DECOMP_ROOT}/pc/src/pc_misc.c"
    "${DECOMP_ROOT}/pc/src/pc_m_card.c"
    "${DECOMP_ROOT}/pc/src/pc_save_bswap.c"
    "${DECOMP_ROOT}/pc/src/pc_settings.c"
    "${DECOMP_ROOT}/pc/src/pc_stubs.c"
    "${DECOMP_ROOT}/pc/src/pc_stubs_cpp.cpp"
    "${DECOMP_ROOT}/pc/src/pc_assets.c"
    "${DECOMP_ROOT}/pc/src/pc_disc.c"
    "${DECOMP_ROOT}/pc/src/pc_pad.c"
    "${DECOMP_ROOT}/pc/src/pc_keybindings.c"
    PROPERTIES COMPILE_OPTIONS "-O2;-w"
)

add_executable(${PROJECT_NAME}
    ${VITA_SOURCES}
    ${CODE_C_SOURCES}
    ${GAME_CXX_SOURCES}
)

target_link_options(${PROJECT_NAME} PRIVATE
    -flto
    -Wl,--gc-sections
    -Wl,--undefined=_newlib_heap_size_user
    -Wl,--undefined=sceUserMainThreadStackSize
)

target_link_libraries(${PROJECT_NAME}
    game_data
    SDL2
    vitaGL vitashark SceShaccCgExt taihen_stub SceShaccCg_stub
    SceDisplay_stub SceCtrl_stub SceAudio_stub SceAudioIn_stub
    SceSysmodule_stub SceGxm_stub SceCommonDialog_stub SceTouch_stub
    SceHid_stub SceMotion_stub ScePower_stub SceAppMgr_stub
    SceAppUtil_stub SceKernelDmacMgr_stub SceKernelThreadMgr_stub
    mathneon pthread m z c stdc++
)

vita_create_self(eboot.bin ${PROJECT_NAME})
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12") # extended memory

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/pic0.png sce_sys/pic0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/../pc/shaders/default.vert shaders/default.vert
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/../pc/shaders/default.frag shaders/default.frag
)
