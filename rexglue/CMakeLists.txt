# pacman - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(pacman LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(PACMAN_SOURCES
    src/main.cpp
    src/spin_hooks.cpp
    src/frame_stats.cpp
    src/playfield.cpp
    src/profiler.cpp
    src/diagnostics.cpp
    src/texture_release.cpp
    src/autodemo.cpp
    src/random_mazes.cpp
)
if(WIN32)
    # Exe icon: pacman.ico, an original design drawn by ../Tools/make_icon.py (no game artwork).
    list(APPEND PACMAN_SOURCES src/pacman.rc)
    # The resource compiler's includes aren't dependency-tracked; rebuild when the icon changes.
    set_source_files_properties(src/pacman.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pacman.ico")
endif()

if(WIN32)
    add_executable(pacman WIN32 ${PACMAN_SOURCES})
else()
    add_executable(pacman ${PACMAN_SOURCES})
endif()

rexglue_setup_target(pacman GPU_PLUGINS xenos)

# CPU level for the recompiled game code. x86-64-v3 (AVX2 generation: Haswell / Zen and later, incl. the Steam
# Deck) brings MOVBE (the big-endian loads/stores everywhere), BMI2 (shifts/rotates) and hardware FMA (the
# game's 1300+ std::fma calls for PowerPC fmadd, exact either way). -ffp-contract=off keeps the compiler from
# fusing separate multiplies and adds on its own, so floating-point results stay those of the Xbox.
# Set PACMAN_CPU_LEVEL=x86-64-v2 for a build that runs on older CPUs.
set(PACMAN_CPU_LEVEL "x86-64-v3" CACHE STRING "-march level for the recompiled game code")
if(TARGET pacman_recomp)
    target_compile_options(pacman_recomp PRIVATE -march=${PACMAN_CPU_LEVEL} -ffp-contract=off)
endif()

# Profile-guided optimisation of the game code: PACMAN_PGO=generate builds an instrumented exe that writes
# counts (LLVM_PROFILE_FILE) on exit; merge them with llvm-profdata and set PACMAN_PGO to the .profdata path.
set(PACMAN_PGO "" CACHE STRING "Profile-guided optimisation: empty, generate, or path to a .profdata")
if(TARGET pacman_recomp AND PACMAN_PGO STREQUAL "generate")
    target_compile_options(pacman_recomp PRIVATE -fprofile-instr-generate)
    target_link_options(pacman PRIVATE -fprofile-instr-generate)
    # The app writes the counts itself on shutdown (pacman_app.h): its exit path skips the runtime's atexit.
    target_compile_definitions(pacman PRIVATE PACMAN_PGO_GENERATE=1)
elseif(TARGET pacman_recomp AND PACMAN_PGO)
    target_compile_options(pacman_recomp PRIVATE -fprofile-instr-use=${PACMAN_PGO}
                           -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
endif()
