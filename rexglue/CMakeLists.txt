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
