cmake_minimum_required(VERSION 3.20)

# Host program for the recompiled game. Build with clang-cl (the generated code needs clang):
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build
project(PacManRecomp C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # must match XenonUtils

if (NOT EXISTS ${CMAKE_SOURCE_DIR}/ppc/ppc_func_mapping.cpp)
    message(FATAL_ERROR "ppc/ is missing - run XenonRecomp first (see CLAUDE.md)")
endif()

# XenonUtils loads the XEX at runtime; only the targets we link get built.
add_subdirectory(tools/xenonRecomp EXCLUDE_FROM_ALL)

file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/ppc/*.cpp)

add_executable(PacManHost
    host/main.cpp
    host/loader.cpp
    host/log.cpp
    host/memory.cpp
    host/stubs.cpp
    host/import_stubs.cpp
    host/kernel/guest_heap.cpp
    host/kernel/memory_imports.cpp
    ${PPC_SOURCES}
)

set_target_properties(PacManHost PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# ppc/ first: XenonUtils also exports a ppc_context.h.
target_include_directories(PacManHost BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/ppc)
target_include_directories(PacManHost PRIVATE ${CMAKE_SOURCE_DIR}/host)

target_compile_options(PacManHost PRIVATE -march=sandybridge -fno-strict-aliasing)
target_link_libraries(PacManHost PRIVATE XenonUtils dbghelp)
