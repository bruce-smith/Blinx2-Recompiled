cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Blinx 2 - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(blinx_2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Diagnostic: after every direct call in the generated code, check that the
# callee restored ebx/esi/edi and popped its return address, and log any
# that did not (recomp_abi_violation_log in the runtime). Off by default:
# it costs a save and a compare per call. -DBLINX2_ABI_CHECK=ON.
option(BLINX2_ABI_CHECK "Check the callee-saved registers after every direct call" OFF)
if(BLINX2_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK)
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# POSIX: the executable must be position independent. Guest memory is mapped
# at its Xbox addresses (0x00010000 up, plus the 128 MB contiguous window), and
# a non-PIE x86-64 Linux executable loads at 0x00400000, in the middle of it.
if(NOT WIN32)
    # The Vulkan backend's glslang is C++: with CXX enabled here CMake links
    # the executable with the C++ driver, which brings in libstdc++.
    enable_language(CXX)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    include(CheckPIESupported)
    check_pie_supported()
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# Full debug info for the kernel library (NV2A executor, kernel bridges), so
# RECOMP_SAMPLE_MS profiles and crash dumps name its static functions instead
# of the nearest public symbol. Code generation is unchanged.
option(BLINX2_KERNEL_DEBUG_INFO "Full debug info (/Zi) for the xbox_kernel library" ON)
if(BLINX2_KERNEL_DEBUG_INFO AND MSVC)
    target_compile_options(xbox_kernel PRIVATE /Zi)
endif()

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Both repos' commits, for the input recording's header (and anything else
# that should say which build produced it), in build_sha.h. Regenerated on
# every build, not at configure time: a relink after a pull without a
# reconfigure used to stamp recordings with the commit of the last configure.
find_package(Git QUIET)
set(BLINX2_SHA_DIR ${CMAKE_CURRENT_BINARY_DIR}/build_sha)
add_custom_target(blinx2_git_sha ALL
    COMMAND ${CMAKE_COMMAND} -DOUT=${BLINX2_SHA_DIR}/build_sha.h
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_sha.cmake
    BYPRODUCTS ${BLINX2_SHA_DIR}/build_sha.h
    COMMENT "Stamping the build with both repos' commits"
    VERBATIM)
add_dependencies(${PROJECT_NAME} blinx2_git_sha)
target_include_directories(${PROJECT_NAME} PRIVATE ${BLINX2_SHA_DIR})
# Where the game's files (default.xbe and the .ipk/.iph archives) are looked
# for when neither RECOMP_GAME_DIR nor a game_files folder next to the exe
# says otherwise (src/main.c, resolve_game_dir).
set(BLINX2_GAME_DIR "E:/Blinx2project/game_files" CACHE PATH
    "Default directory of the extracted game files (default.xbe)")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    BLINX2_GAME_DIR_DEFAULT="${BLINX2_GAME_DIR}")

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    # Windows SDK libraries needed at link time
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        xinput
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
else()
    # Export every symbol, so the fault handler's dladdr names the generated
    # sub_XXXXXXXX a host RIP is in (the POSIX stand-in for the PDB).
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
    # The generated C is 3 M lines of mechanical code: its warnings are noise.
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # Public-symbol PDB (relink only, no /Zi needed) so the VEH crash dump
        # and the RECOMP_SAMPLE_MS sampler can name sub_XXXXXXXX frames.
        /DEBUG
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
