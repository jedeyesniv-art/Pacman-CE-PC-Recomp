#pragma once

#include <cstddef>
#include <cstdint>

// The whole 4 GB guest address space is one host allocation: guest address x lives at g_base + x.
// The recompiled function table also lives inside it, right after the image (see PPC_LOOKUP_FUNC).
extern uint8_t* g_base;

constexpr uint64_t GuestMemorySize = 0x100000000ull;

// Guest memory the host hands out for its own structures (stacks, PCR, data imports).
// The game's allocators (NtAllocateVirtualMemory, MmAllocatePhysicalMemoryEx) must stay out of it.
// It ends below 0x7F000000: that range is device registers (the GPU's are at 0x7FC80000).
constexpr uint32_t HostRegionBegin = 0x7C000000;
constexpr uint32_t HostRegionEnd = 0x7F000000;
constexpr uint32_t GpuRegisterBase = 0x7FC80000;

bool GuestMemoryInit();

// Zero-filled bump allocation from the host region; returns a guest address. Stops the host when full.
uint32_t GuestAlloc(uint32_t size, uint32_t align = 16);

// Allocates a guest stack with a no-access guard region below it, so an overflow faults right away.
// Returns the low end of the usable stack.
constexpr uint32_t GuestStackGuardSize = 0x10000;
uint32_t GuestAllocStack(uint32_t size);

// True if a guest address lies in a stack guard region.
bool IsGuestStackGuard(uint32_t address);

inline bool IsGuestHostPointer(const void* p)
{
    return p >= g_base && p < g_base + GuestMemorySize;
}

inline uint32_t GuestLoadU32(uint32_t address)
{
    return __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + address));
}

inline void GuestStoreU32(uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(g_base + address) = __builtin_bswap32(value);
}
