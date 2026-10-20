# pacman - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(pacman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(PACMAN_SOURCES
    src/main.cpp
    src/spin_hooks.cpp
    src/frame_stats.cpp
)

if(WIN32)
    add_executable(pacman WIN32 ${PACMAN_SOURCES})
else()
    add_executable(pacman ${PACMAN_SOURCES})
endif()

rexglue_setup_target(pacman GPU_PLUGINS xenos)
