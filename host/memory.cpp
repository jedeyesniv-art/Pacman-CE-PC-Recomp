#include "memory.h"
#include "log.h"

#include <ppc_config.h>

#include <cstdlib>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static_assert(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2 <= GuestMemorySize, "function table must fit in guest memory");
static_assert(HostRegionEnd <= PPC_IMAGE_BASE, "host region must not overlap the image");

uint8_t* g_base;

static std::mutex g_allocMutex;
static uint32_t g_allocNext = HostRegionBegin;

bool GuestMemoryInit()
{
    // Committing all 4 GB up front only costs commit charge; pages are backed when first touched.
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, GuestMemorySize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (g_base == nullptr)
    {
        LogPrint("error: could not allocate 4 GB of guest memory (VirtualAlloc error %lu)", GetLastError());
        return false;
    }

    // Make guest null-pointer accesses fault instead of silently reading zeroes.
    DWORD oldProtect;
    VirtualProtect(g_base, 0x1000, PAGE_NOACCESS, &oldProtect);

    LogPrint("guest memory at host %p", static_cast<void*>(g_base));
    return true;
}

uint32_t GuestAlloc(uint32_t size, uint32_t align)
{
    std::lock_guard lock(g_allocMutex);
    uint32_t address = (g_allocNext + align - 1) & ~(align - 1);
    if (uint64_t(address) + size > HostRegionEnd)
    {
        LogPrint("error: host guest-memory region exhausted (requested 0x%X bytes)", size);
        LogFlush();
        std::_Exit(4);
    }

    g_allocNext = address + size;
    return address;
}
