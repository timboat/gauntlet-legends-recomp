cmake_minimum_required(VERSION 3.20)
project(GauntletRecomp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EXT ${CMAKE_SOURCE_DIR}/../deps)
set(RECOMP ${CMAKE_SOURCE_DIR}/../recomp)
set(SDL2_DIR_VC ${CMAKE_SOURCE_DIR}/../deps/sdl2/SDL2-2.30.9)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-everything")
endif()
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR NOMINMAX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# RT64 (static) and the N64 runtime
set(RT64_STATIC TRUE)
add_compile_definitions(HLSL_CPU)
add_subdirectory(${EXT}/rt64 ${CMAKE_BINARY_DIR}/rt64)
add_subdirectory(${EXT}/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

set(RECOMP_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
    ${EXT}/N64ModernRuntime/ultramodern/include
    ${EXT}/N64ModernRuntime/librecomp/include
    ${EXT}/N64ModernRuntime/N64Recomp/include
)

# Recompiled game code
add_library(RecompiledFuncs STATIC)
file(GLOB FUNC_C_SOURCES ${RECOMP}/RecompiledFuncs/*.c)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES})
target_include_directories(RecompiledFuncs PRIVATE ${RECOMP_INCLUDES})
option(GAUNTLET_TRACE "Record recompiled function entries for crash reports" ON)
if (GAUNTLET_TRACE)
    add_compile_definitions(GAUNTLET_TRACE)
endif()
target_compile_options(RecompiledFuncs PRIVATE -fno-strict-aliasing -Wno-unused-variable -Wno-implicit-function-declaration)

add_executable(GauntletRecomp
    src/main.cpp
    src/render_context.cpp
    src/register_overlays.cpp
    src/game_hooks.cpp
    src/mips_interp.cpp
    src/input.cpp
    src/crash_trace.cpp
    ${RECOMP}/rsp/audio_ucode.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../gfx/n64gfx.cpp
)
target_include_directories(GauntletRecomp PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../gfx
    ${EXT}/N64ModernRuntime/thirdparty
    ${RECOMP_INCLUDES}
    ${RECOMP}/RecompiledFuncs
    ${EXT}/N64ModernRuntime/thirdparty/concurrentqueue
    ${EXT}/N64ModernRuntime/thirdparty/miniz
    ${EXT}/rt64/src/contrib
    ${EXT}/rt64/src/contrib/hlslpp/include
    ${EXT}/rt64/src/contrib/dxc/inc
    ${EXT}/rt64/src
    ${EXT}/rt64/src/rhi
    ${EXT}/rt64/src/render
    ${SDL2_DIR_VC}/include
)
target_compile_options(GauntletRecomp PRIVATE -march=nehalem -fno-strict-aliasing -fms-extensions)
target_link_directories(GauntletRecomp PRIVATE ${SDL2_DIR_VC}/lib/x64)
target_link_libraries(GauntletRecomp PRIVATE RecompiledFuncs librecomp ultramodern rt64 SDL2 Winmm.lib)
target_link_options(GauntletRecomp PRIVATE /OPT:NOICF)

add_custom_command(TARGET GauntletRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SDL2_DIR_VC}/lib/x64/SDL2.dll"
        "${EXT}/rt64/src/contrib/dxc/bin/x64/dxil.dll"
        "${EXT}/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
        $<TARGET_FILE_DIR:GauntletRecomp>)
