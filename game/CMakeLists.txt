cmake_minimum_required(VERSION 3.21)
project(lcs_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated C++ from the user's own SLUS_215.90 (tools/recompile.py). Never committed.
set(LCS_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/ee" CACHE PATH "Recompiled EE code")

set(PS2X_LTO "THIN" CACHE STRING "Link-time optimisation for the game: THIN (default, the game to play), OFF (quick dev builds), FULL")
set(PS2X_PGO "" CACHE STRING "Profile-guided optimisation: empty (off), GEN (instrumented), or a .profdata path (tools/build.py --opt)")
set(PS2X_CPU "AVX" CACHE STRING "The CPU the build runs on: AVX (default, Sandy Bridge / Bulldozer and newer), AVX2 (Haswell / Zen and newer), or SSE4 (no AVX)")
set_property(CACHE PS2X_CPU PROPERTY STRINGS AVX2 AVX SSE4)
# The portable kit pins its baseline, including updates from older launchers
# that reuse a CMake cache originally configured with AVX2.
include("${CMAKE_CURRENT_SOURCE_DIR}/lcs-release-cpu.cmake" OPTIONAL)
option(LCS_BUILD_TOOLS "Build the PS2Recomp recompiler/analyzer" ON)
# [lcs] ON: the diagnosis build (build/diag, every diagnostic hook and log line); OFF: the performance build
# (build/release) without the pure diagnostic hooks and the log lines that run during play (src/lcs/lcs_diag.h).
option(LCS_DIAGNOSTICS "Diagnostic hooks and play-time log lines (OFF for the performance build)" ON)
if(LCS_DIAGNOSTICS)
    set(_lcs_diag_value 1)
else()
    set(_lcs_diag_value 0)
endif()
# [lcs] ON: the experimental pause-menu video page (build/menu -> build/runtime-menu, src/lcs/lcs_menu.cpp); release and
# diag stay without it until it works
option(LCS_MENU_VIDEO "Experimental: the pause menu's display page as the video settings" OFF)
if(LCS_MENU_VIDEO)
    set(_lcs_menu_value 1)
else()
    set(_lcs_menu_value 0)
endif()

set(PS2X_BUILD_RECOMP ${LCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER ${LCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)

# Linux (docs/LINUX.md) and Windows' llvm-mingw (DECISIONS.md 2026-11-03): clang 19+ only - the same compiler as
# clang-cl, and PS2_GUEST_CC needs its preserve_none. Linked with lld (ThinLTO).
if(NOT MSVC)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
        message(FATAL_ERROR "The game needs clang 19 or newer (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). "
                            "python tools/build.py picks it up (docs/LINUX.md; Windows: llvm-mingw, downloaded)")
    endif()
    add_link_options(-fuse-ld=lld)
endif()
# llvm-mingw: mingw-w64's headers default to Windows 7, the Windows SDK's to Windows 10 - the same APIs as clang-cl's
# build. Every program is linked -static: libc++, libunwind and winpthread inside, only the system's UCRT left.
if(MINGW)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
    add_link_options(-static)
endif()
# Linux: compile and ThinLTO jobs follow the memory - a unity batch of generated code takes up to ~2 GB to compile.
if(NOT WIN32)
    cmake_host_system_information(RESULT _lcs_mem_mb QUERY TOTAL_PHYSICAL_MEMORY)
    cmake_host_system_information(RESULT _lcs_cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _lcs_jobs "${_lcs_mem_mb} / 2048")
    if(_lcs_jobs LESS 1)
        set(_lcs_jobs 1)
    endif()
    if(_lcs_jobs GREATER _lcs_cores)
        set(_lcs_jobs ${_lcs_cores})
    endif()
    set(LCS_COMPILE_JOBS ${_lcs_jobs} CACHE STRING "Parallel compile jobs (default: one per 2 GB of memory, at most one per core)")
    set(PS2X_LTO_JOBS ${_lcs_jobs} CACHE STRING "Parallel ThinLTO backend jobs at the game's link")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS lcs_compile=${LCS_COMPILE_JOBS})
    set(CMAKE_JOB_POOL_COMPILE lcs_compile)
    message(STATUS "Linux build: ${LCS_COMPILE_JOBS} compile jobs, ${PS2X_LTO_JOBS} ThinLTO jobs (${_lcs_mem_mb} MB memory)")
endif()

set(LCS_HAVE_GENERATED OFF)
if(EXISTS "${LCS_GENERATED_DIR}/register_functions.cpp")
    set(LCS_HAVE_GENERATED ON)
endif()
set(PS2X_BUILD_RUNTIME ${LCS_HAVE_GENERATED} CACHE BOOL "" FORCE)

file(GLOB_RECURSE LCS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM LCS_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")
# The platform layer, RHI and renderer are their own libraries (src/*/CMakeLists.txt), linked into the runtime.
list(FILTER LCS_SOURCES EXCLUDE REGEX "/src/(platform|rhi|render)/")
# VU1 microprograms compiled to C++ by tools/vu1_recomp.py (from the user's ELF; see game/lcs/vu1_uploads.txt)
if(EXISTS "${CMAKE_SOURCE_DIR}/generated/vu1/vu1_compiled.cpp")
    list(APPEND LCS_SOURCES "${CMAKE_SOURCE_DIR}/generated/vu1/vu1_compiled.cpp")
endif()

set(PS2X_RUNNER_SOURCE_DIR "${LCS_GENERATED_DIR}" CACHE PATH "" FORCE)
set(PS2X_RUNNER_EXTRA_SOURCES "${LCS_SOURCES}" CACHE STRING "" FORCE)
set(PS2X_RUNNER_MAIN "${CMAKE_SOURCE_DIR}/src/main.cpp" CACHE FILEPATH "" FORCE)

set(PS2X_PARALLEL_GS_DIR "${CMAKE_SOURCE_DIR}/third_party/parallel-gs" CACHE PATH "" FORCE)
if(LCS_HAVE_GENERATED)
    add_subdirectory(src/platform)
    add_subdirectory(src/rhi)
    add_subdirectory(src/render)
    add_library(lcs_host INTERFACE)
    target_link_libraries(lcs_host INTERFACE lcs_platform lcs_render)
    set(PS2X_HOST_PLATFORM_TARGET lcs_host CACHE STRING "" FORCE)
endif()
add_subdirectory(third_party/PS2Recomp)

if(LCS_HAVE_GENERATED)
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "LCSrecomp")
    # The main build tree (build/release) puts LCSrecomp.exe, its DLLs and lcs.ini at the top of build/ instead of
    # build/release/third_party/PS2Recomp/ps2xRuntime; other trees (PGO training's build/pgo-gen) keep theirs.
    # LCS_GAME_DIR: the build folder chosen elsewhere (tools/toolchain.py passes it for the main tree; docs/LAUNCHER.md).
    set(LCS_GAME_DIR "" CACHE PATH "Where LCSrecomp.exe, its DLLs and lcs.ini go (empty: build/ for build/release)")
    string(TOLOWER "${CMAKE_BINARY_DIR}" _lcs_binary_dir)
    string(TOLOWER "${CMAKE_SOURCE_DIR}/build/release" _lcs_main_tree)
    if(LCS_GAME_DIR)
        set_target_properties(ps2EntryRunner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${LCS_GAME_DIR}")
    elseif(_lcs_binary_dir STREQUAL _lcs_main_tree)
        set_target_properties(ps2EntryRunner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
    endif()
    target_include_directories(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(ps2EntryRunner PRIVATE lcs_host) # main.cpp: settings menu (Dear ImGui), presenter aspect
    # Windows: our manifest in place of the linker's, for the UTF-8 code page (non-ASCII folders; src/lcs.manifest)
    if(WIN32)
        target_sources(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src/lcs.rc")
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/lcs.rc" TARGET_DIRECTORY ps2EntryRunner
            PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/lcs.manifest;${CMAKE_SOURCE_DIR}/src/lcs.ico")
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE /MANIFEST:NO)  # lld in mingw mode makes none of its own
        endif()
    endif()
    # Our own sources aren't generated code; keep them out of the unity batches.
    set_source_files_properties(${LCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE)
    # [lcs] only our sources and the runtime library: the generated code does not see it (no rebuild of it)
    set_property(SOURCE ${LCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp" TARGET_DIRECTORY ps2EntryRunner APPEND PROPERTY
        COMPILE_DEFINITIONS LCS_DIAGNOSTICS=${_lcs_diag_value} LCS_MENU_VIDEO=${_lcs_menu_value})
    target_compile_definitions(ps2_runtime PRIVATE LCS_DIAGNOSTICS=${_lcs_diag_value})
    message(STATUS "LCS_DIAGNOSTICS=${LCS_DIAGNOSTICS} LCS_MENU_VIDEO=${LCS_MENU_VIDEO}")
else()
    message(STATUS "No generated code in ${LCS_GENERATED_DIR}: building tools only. Run python tools/build.py --recompile.")
endif()

# Debug symbols for the runtime and our code (not the 240 MB of generated code),
# so the sampling profiler and debuggers can name host functions.
option(LCS_RUNTIME_DEBUG_INFO "Emit CodeView debug info for runtime + launcher" ON)
if(LCS_RUNTIME_DEBUG_INFO AND MSVC)
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE /Z7)
        endif()
    endforeach()
    if(LCS_HAVE_GENERATED)
        set_source_files_properties(${LCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "/Z7")
        target_link_options(ps2EntryRunner PRIVATE /DEBUG)
        target_link_libraries(ps2EntryRunner PRIVATE dbghelp)
    endif()
elseif(LCS_RUNTIME_DEBUG_INFO AND MINGW)
    # llvm-mingw: CodeView in a PDB, as clang-cl's /Z7 /DEBUG - dbghelp (src/debug: crash handler, profiler) and
    # tools/minidbg.py read it the same way
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -g -gcodeview)
        endif()
    endforeach()
    if(LCS_HAVE_GENERATED)
        set_source_files_properties(${LCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-g;-gcodeview")
        # the PDB by its name only in the exe (no build folder path inside a release)
        target_link_options(ps2EntryRunner PRIVATE -g -Wl,--pdb= "-Wl,-Xlink=/pdbaltpath:%_PDB%")
        target_link_libraries(ps2EntryRunner PRIVATE dbghelp)
    endif()
elseif(LCS_RUNTIME_DEBUG_INFO)
    # Linux: line tables for the crash handler / profiler (src/debug, symbolized with llvm-symbolizer); frame pointers
    # for their backtraces
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -gline-tables-only -fno-omit-frame-pointer)
        endif()
    endforeach()
    if(LCS_HAVE_GENERATED)
        set_source_files_properties(${LCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-gline-tables-only;-fno-omit-frame-pointer")
        target_link_options(ps2EntryRunner PRIVATE -rdynamic)
    endif()
endif()
# llvm-mingw: the profiler's __try / __except (src/debug/profiler.cpp) need the Microsoft extensions clang-cl has on
# (here, after the debug info's COMPILE_OPTIONS above, which would replace them)
if(MINGW AND LCS_HAVE_GENERATED)
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/debug/profiler.cpp" TARGET_DIRECTORY ps2EntryRunner APPEND PROPERTY
                 COMPILE_OPTIONS -fms-extensions)
endif()
