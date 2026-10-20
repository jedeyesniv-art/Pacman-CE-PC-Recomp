#pragma once

#include <cstdint>

// NTSTATUS values returned to the guest.
constexpr uint32_t STATUS_SUCCESS = 0x00000000;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_CONFLICTING_ADDRESSES = 0xC0000018;
constexpr uint32_t STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;

// Xbox 360 allocation type flags (values as in Xenia's xbox.h).
constexpr uint32_t X_MEM_COMMIT = 0x00001000;
constexpr uint32_t X_MEM_RESERVE = 0x00002000;
constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
constexpr uint32_t X_MEM_RELEASE = 0x00008000;
constexpr uint32_t X_MEM_FREE = 0x00010000;
constexpr uint32_t X_MEM_PRIVATE = 0x00020000;
constexpr uint32_t X_MEM_TOP_DOWN = 0x00100000;
constexpr uint32_t X_MEM_NOZERO = 0x00800000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000; // 64 KB pages
constexpr uint32_t X_MEM_HEAP = 0x40000000;
constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;
