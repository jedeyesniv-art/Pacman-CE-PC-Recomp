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

# Exe icon from the owner's own game files (game_icon.png from the Xbox package). It is game artwork, so it is
# never committed: the .ico and .rc are generated in the build folder, and without the PNG there's no icon.
set(PACMAN_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/../private/game_icon.png" CACHE FILEPATH
    "PNG to use as the exe icon (optional)")
find_package(Python3 COMPONENTS Interpreter)
if(WIN32 AND EXISTS "${PACMAN_ICON_PNG}" AND Python3_FOUND)
    set(PACMAN_ICON_ICO "${CMAKE_CURRENT_BINARY_DIR}/pacman.ico")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/../Tools/png_to_ico.py"
                "${PACMAN_ICON_PNG}" "${PACMAN_ICON_ICO}"
        RESULT_VARIABLE PACMAN_ICON_RESULT)
    if(PACMAN_ICON_RESULT EQUAL 0)
        enable_language(RC)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pacman_icon.rc" "1 ICON \"${PACMAN_ICON_ICO}\"\n")
        target_sources(pacman PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/pacman_icon.rc")
        message(STATUS "Exe icon: ${PACMAN_ICON_PNG}")
    endif()
else()
    message(STATUS "Exe icon: none (${PACMAN_ICON_PNG} not found)")
endif()
