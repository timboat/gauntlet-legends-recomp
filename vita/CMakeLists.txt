# PS Vita build of Gauntlet Legends: Recompiled -> gauntlet.vpk (title id GLEG00001).
#   vita\build.bat
# Uses the same recompiled game code, runtime (N64ModernRuntime) and game hooks as the PC build in ../port,
# with a Vita frontend (src/main_vita.cpp), a mod-free start-up (src/recomp_vita.cpp) and a 32 MB rdram mask.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{VITASDK})
    set(ENV{VITASDK} "${CMAKE_CURRENT_LIST_DIR}/../deps/vitasdk")
  endif()
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(gauntletvita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Gauntlet Legends")
set(VITA_TITLEID  "GLEG00001")
set(VITA_VERSION  "01.00")

set(GL ${CMAKE_CURRENT_LIST_DIR}/..)
set(EXT ${GL}/deps)
set(RT ${EXT}/N64ModernRuntime)
set(RECOMP ${GL}/recomp)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OPT "-O2 -mcpu=cortex-a9 -mfpu=neon -mtune=cortex-a9 -fno-strict-aliasing")
# -fexceptions: the runtime ends game threads by throwing through recompiled (C) frames, which need unwind tables.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OPT} -fexceptions")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OPT} -Wno-psabi")
add_compile_definitions(RDRAM_MASK=0x01FFFFFF)
if(GAUNTLET_POISON_NEW)
  add_compile_definitions(GAUNTLET_POISON_NEW)
endif()

set(INCLUDES
    ${GL}/port/include
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${GL}/gfx
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${RT}/ultramodern/include
    ${RT}/librecomp/include
    ${RT}/librecomp/include/librecomp
    ${RT}/librecomp/src
    ${RT}/N64Recomp/include
    ${RT}/thirdparty
    ${RT}/thirdparty/concurrentqueue
    ${RT}/thirdparty/sse2neon
    ${RT}/thirdparty/o1heap
    ${RT}/thirdparty/miniz
    ${RECOMP}/RecompiledFuncs
)

# Recompiled game code (large; kept in its own library so frontend edits don't recompile it).
file(GLOB FUNC_C_SOURCES ${RECOMP}/RecompiledFuncs/*.c)
add_library(RecompiledFuncs STATIC ${FUNC_C_SOURCES})
target_include_directories(RecompiledFuncs PRIVATE ${INCLUDES})
target_compile_options(RecompiledFuncs PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable -Wno-implicit-function-declaration -w)

set(ULTRAMODERN_SRC
    ${RT}/ultramodern/src/audio.cpp
    ${RT}/ultramodern/src/error_handling.cpp
    ${RT}/ultramodern/src/events.cpp
    ${RT}/ultramodern/src/input.cpp
    ${RT}/ultramodern/src/mesgqueue.cpp
    ${RT}/ultramodern/src/misc_ultra.cpp
    ${RT}/ultramodern/src/renderer_context.cpp
    ${RT}/ultramodern/src/rsp.cpp
    ${RT}/ultramodern/src/scheduling.cpp
    ${RT}/ultramodern/src/threadqueue.cpp
    ${RT}/ultramodern/src/threads.cpp
    ${RT}/ultramodern/src/timer.cpp
    ${RT}/ultramodern/src/ultrainit.cpp
)
set(LIBRECOMP_SRC
    ${RT}/librecomp/src/ai.cpp
    ${RT}/librecomp/src/cont.cpp
    ${RT}/librecomp/src/dp.cpp
    ${RT}/librecomp/src/eep.cpp
    ${RT}/librecomp/src/files.cpp
    ${RT}/librecomp/src/flash.cpp
    ${RT}/librecomp/src/heap.cpp
    ${RT}/librecomp/src/math_routines.cpp
    ${RT}/librecomp/src/overlays.cpp
    ${RT}/librecomp/src/pak.cpp
    ${RT}/librecomp/src/pi.cpp
    ${RT}/librecomp/src/rsp.cpp
    ${RT}/librecomp/src/sp.cpp
    ${RT}/librecomp/src/ultra_stubs.cpp
    ${RT}/librecomp/src/ultra_translation.cpp
    ${RT}/librecomp/src/vi.cpp
    ${RT}/thirdparty/o1heap/o1heap/o1heap.c
    ${RT}/thirdparty/miniz/miniz.c
    ${RT}/thirdparty/miniz/miniz_tdef.c
    ${RT}/thirdparty/miniz/miniz_tinfl.c
)
set(GAME_SRC
    ${GL}/port/src/game_hooks.cpp
    ${GL}/port/src/register_overlays.cpp
    ${GL}/port/src/mips_interp.cpp
    ${RECOMP}/rsp/audio_ucode.cpp
    src/main_vita.cpp
    src/vu_check.cpp
    src/vu_check_sisd.cpp
    src/vu_check_simd.cpp
    src/recomp_vita.cpp
    src/render_gxm.cpp
    ${GL}/gfx/n64gfx.cpp
)

# The renderer does all transform, lighting and combiner maths on the CPU: let GCC vectorise it for NEON.
set_source_files_properties(src/render_gxm.cpp ${GL}/gfx/n64gfx.cpp PROPERTIES
  COMPILE_OPTIONS "-O3;-ffast-math;-fno-finite-math-only")
# Audio microcode blocks record their RSP address for the perf.txt "rsp:" profile.
# The microcode is one huge function: raise GCC's growth limits so the vector-unit helpers are inlined instead of
# called (out of line they pass every vector through memory).
set_source_files_properties(${RECOMP}/rsp/audio_ucode.cpp PROPERTIES COMPILE_DEFINITIONS RSP_PROF_LABEL
  COMPILE_OPTIONS "--param=large-function-growth=100000;--param=large-function-insns=1000000;--param=inline-unit-growth=100000;--param=max-inline-insns-single=10000;--param=large-stack-frame-growth=100000")
add_executable(gauntlet ${ULTRAMODERN_SRC} ${LIBRECOMP_SRC} ${GAME_SRC})
target_include_directories(gauntlet PRIVATE ${INCLUDES})
target_link_libraries(gauntlet
  RecompiledFuncs
  vita2d
  vitashark SceShaccCgExt SceShaccCg_stub_weak taihen_stub
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub SceAudio_stub
  SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub ScePower_stub
  SceRtc_stub SceKernelDmacMgr_stub SceLibKernel_stub SceIofilemgr_stub
  -Wl,--whole-archive pthread -Wl,--no-whole-archive m stdc++)

# vita-elf-create appends ~3.6 KB of module info after the code; leave headroom or it fails on "segment overlaps".
target_link_options(gauntlet PRIVATE -Wl,--defsym=__sce_headroom=0x2000)
vita_create_self(eboot.bin gauntlet UNSAFE)
# bubble icon + LiveArea: made from grabs of the game's intro by make_livearea.py (not shipped with the source)
set(LIVEAREA_FILES)
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/sce_sys/icon0.png)
  set(LIVEAREA_FILES
    FILE ${CMAKE_CURRENT_LIST_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
endif()
vita_create_vpk(gauntlet.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  ${LIVEAREA_FILES}
)
