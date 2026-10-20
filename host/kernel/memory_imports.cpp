// Virtual and physical memory imports (Nt*VirtualMemory, Mm*PhysicalMemory and queries).
// Heap layout and argument order follow Xenia (xboxkrnl_memory.cc).
#include "guest_heap.h"
#include "xbox.h"
#include "log.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <algorithm>
#include <atomic>

// Calls logged per import; enough to see the startup pattern without flooding the log.
constexpr uint32_t LoggedCalls = 32;

static GuestHeap g_virtual4k("virtual 4K", 0x00010000, 0x40000000, 0x1000);
static GuestHeap g_virtual64k("virtual 64K", 0x40000000, HostRegionBegin, 0x10000);

// Physical memory windows. Real hardware maps all three onto the same 512 MB of RAM; here they are
// separate guest ranges, so the same physical page is not visible through two windows.
static GuestHeap g_physical64k("physical 64K", 0xA0000000, 0xC0000000, 0x10000);
static GuestHeap g_physical16m("physical 16M", 0xC0000000, 0xE0000000, 0x1000000);
static GuestHeap g_physical4k("physical 4K", 0xE0000000, 0x100000000ull, 0x1000);

static GuestHeap* const g_virtualHeaps[] = { &g_virtual4k, &g_virtual64k };
static GuestHeap* const g_allHeaps[] = { &g_virtual4k, &g_virtual64k, &g_physical64k, &g_physical16m, &g_physical4k };

template <size_t N>
static GuestHeap* HeapContaining(GuestHeap* const (&heaps)[N], uint32_t address)
{
    for (GuestHeap* heap : heaps)
    {
        if (heap->Contains(address))
            return heap;
    }
    return nullptr;
}

static bool ShouldLog(std::atomic<uint32_t>& calls)
{
    return calls.fetch_add(1) < LoggedCalls;
}

static uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect)
{
    if ((type & (X_MEM_COMMIT | X_MEM_RESERVE)) == 0 || size == 0)
        return STATUS_INVALID_PARAMETER;

    if (base != 0)
    {
        GuestHeap* heap = HeapContaining(g_virtualHeaps, base);
        if (heap == nullptr)
            return STATUS_INVALID_PARAMETER;

        const uint32_t page = heap->PageSize();
        const uint32_t start = base & ~(page - 1);
        const uint64_t end = (uint64_t(base) + size + page - 1) & ~uint64_t(page - 1);
        const uint32_t length = uint32_t(end - start);

        if (type & X_MEM_RESERVE)
        {
            if (!heap->AllocateFixed(start, length, (type & X_MEM_COMMIT) != 0, protect))
                return STATUS_CONFLICTING_ADDRESSES;
        }
        else if (!heap->Commit(start, length, protect))
        {
            return STATUS_MEMORY_NOT_ALLOCATED;
        }

        base = start;
        size = length;
        return STATUS_SUCCESS;
    }

    // No address: reserve anywhere (MEM_COMMIT alone also reserves, as on Windows).
    GuestHeap& heap = (type & (X_MEM_LARGE_PAGES | X_MEM_16MB_PAGES)) ? g_virtual64k : g_virtual4k;
    const uint32_t alignment = (type & X_MEM_16MB_PAGES) ? 0x1000000 : heap.PageSize();
    const uint32_t address = heap.Allocate(size, alignment, (type & X_MEM_COMMIT) != 0, (type & X_MEM_TOP_DOWN) != 0, protect);
    if (address == 0)
        return STATUS_NO_MEMORY;

    base = address;
    size = heap.AllocationSize(address);
    return STATUS_SUCCESS;
}

// NTSTATUS NtAllocateVirtualMemory(u32* base, u32* size, u32 type, u32 protect, u32 debugMemory)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    static std::atomic<uint32_t> calls;
    const uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32, protect = ctx.r6.u32;
    const uint32_t requestedBase = GuestLoadU32(basePtr), requestedSize = GuestLoadU32(sizePtr);

    uint32_t regionBase = requestedBase, regionSize = requestedSize;
    const uint32_t status = AllocateVirtual(regionBase, regionSize, type, protect);
    if (status == STATUS_SUCCESS)
    {
        GuestStoreU32(basePtr, regionBase);
        GuestStoreU32(sizePtr, regionSize);
    }

    if (ShouldLog(calls) || status != STATUS_SUCCESS)
        LogPrint("[mem] NtAllocateVirtualMemory(base=%08X size=%08X type=%08X protect=%X) -> %08X base=%08X size=%08X",
            requestedBase, requestedSize, type, protect, status, regionBase, regionSize);
    ctx.r3.u64 = status;
}

// NTSTATUS NtFreeVirtualMemory(u32* base, u32* size, u32 type, u32 debugMemory)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    static std::atomic<uint32_t> calls;
    const uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    const uint32_t address = GuestLoadU32(basePtr), size = GuestLoadU32(sizePtr);

    uint32_t status = STATUS_INVALID_PARAMETER;
    uint32_t freedBase = address, freedSize = 0;
    if (GuestHeap* heap = HeapContaining(g_virtualHeaps, address))
    {
        const uint32_t page = heap->PageSize();
        if (type & X_MEM_RELEASE)
        {
            freedBase = address & ~(page - 1);
            freedSize = heap->Release(freedBase);
            status = freedSize != 0 ? STATUS_SUCCESS : STATUS_MEMORY_NOT_ALLOCATED;
        }
        else if (type & X_MEM_DECOMMIT)
        {
            // A zero size decommits from the address to the end of the allocation.
            freedBase = address & ~(page - 1);
            const uint32_t length = size != 0
                ? uint32_t(((uint64_t(address) + size + page - 1) & ~uint64_t(page - 1)) - freedBase)
                : heap->AllocationBase(address) + heap->AllocationSize(address) - freedBase;
            if (heap->AllocationSize(address) != 0 && heap->Decommit(freedBase, length))
            {
                freedSize = length;
                status = STATUS_SUCCESS;
            }
            else
            {
                status = STATUS_MEMORY_NOT_ALLOCATED;
            }
        }
    }

    if (status == STATUS_SUCCESS)
    {
        GuestStoreU32(basePtr, freedBase);
        GuestStoreU32(sizePtr, freedSize);
    }

    if (ShouldLog(calls) || status != STATUS_SUCCESS)
        LogPrint("[mem] NtFreeVirtualMemory(base=%08X size=%08X type=%08X) -> %08X base=%08X size=%08X",
            address, size, type, status, freedBase, freedSize);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryVirtualMemory(u32 address, MEMORY_BASIC_INFORMATION* info, u32 debugMemory)
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    static std::atomic<uint32_t> calls;
    const uint32_t address = ctx.r3.u32, infoPtr = ctx.r4.u32;

    GuestHeap::Info info{};
    GuestHeap* heap = HeapContaining(g_allHeaps, address);
    const uint32_t status = heap != nullptr && heap->Query(address, info) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    if (status == STATUS_SUCCESS)
    {
        // X_MEMORY_BASIC_INFORMATION: seven big-endian u32 fields.
        GuestStoreU32(infoPtr + 0x00, info.baseAddress);
        GuestStoreU32(infoPtr + 0x04, info.allocationBase);
        GuestStoreU32(infoPtr + 0x08, info.allocationProtect);
        GuestStoreU32(infoPtr + 0x0C, info.regionSize);
        GuestStoreU32(infoPtr + 0x10, info.state);
        GuestStoreU32(infoPtr + 0x14, info.protect);
        GuestStoreU32(infoPtr + 0x18, info.type);
    }

    if (ShouldLog(calls) || status != STATUS_SUCCESS)
        LogPrint("[mem] NtQueryVirtualMemory(%08X) -> %08X base=%08X size=%08X state=%X", address, status,
            info.baseAddress, info.regionSize, info.state);
    ctx.r3.u64 = status;
}

// u32 MmAllocatePhysicalMemoryEx(u32 flags, u32 size, u32 protect, u32 minAddress, u32 maxAddress, u32 alignment)
// Returns the virtual address of the allocation in the physical window matching the page size, or 0.
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    static std::atomic<uint32_t> calls;
    const uint32_t flags = ctx.r3.u32, size = ctx.r4.u32, protect = ctx.r5.u32;
    const uint32_t minAddress = ctx.r6.u32, maxAddress = ctx.r7.u32, alignment = ctx.r8.u32;

    GuestHeap& heap = (protect & X_MEM_LARGE_PAGES) ? g_physical64k : (protect & X_MEM_16MB_PAGES) ? g_physical16m : g_physical4k;

    // min/max are physical addresses (0 - 512 MB); map them into the window.
    const uint64_t low = uint64_t(heap.Begin()) + std::min<uint32_t>(minAddress, 0x1FFFFFFF);
    const uint64_t high = uint64_t(heap.Begin()) + uint64_t(std::min<uint32_t>(maxAddress, 0x1FFFFFFF)) + 1;
    const uint32_t align = std::max(alignment, heap.PageSize());
    const uint32_t address = heap.Allocate(size, align, true, true, protect, uint32_t(low), high);

    if (ShouldLog(calls) || address == 0)
        LogPrint("[mem] MmAllocatePhysicalMemoryEx(flags=%X size=%08X protect=%08X min=%08X max=%08X align=%X) -> %08X (%s)",
            flags, size, protect, minAddress, maxAddress, alignment, address, heap.Name());
    ctx.r3.u64 = address;
}

// void MmFreePhysicalMemory(u32 type, u32 address)
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    static std::atomic<uint32_t> calls;
    const uint32_t type = ctx.r3.u32, address = ctx.r4.u32;

    GuestHeap* heap = HeapContaining(g_allHeaps, address);
    const uint32_t freed = heap != nullptr ? heap->Release(address & ~(heap->PageSize() - 1)) : 0;

    if (ShouldLog(calls) || freed == 0)
        LogPrint("[mem] MmFreePhysicalMemory(type=%X address=%08X) -> freed %08X", type, address, freed);
}

// u32 MmQueryAllocationSize(u32 address)
PPC_FUNC(__imp__MmQueryAllocationSize)
{
    static std::atomic<uint32_t> calls;
    const uint32_t address = ctx.r3.u32;

    GuestHeap* heap = HeapContaining(g_allHeaps, address);
    const uint32_t size = heap != nullptr ? heap->AllocationSize(address) : 0;

    if (ShouldLog(calls))
        LogPrint("[mem] MmQueryAllocationSize(%08X) -> %08X", address, size);
    ctx.r3.u64 = size;
}

// u32 MmQueryAddressProtect(u32 address): page protection if committed, else 0.
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    static std::atomic<uint32_t> calls;
    const uint32_t address = ctx.r3.u32;

    GuestHeap* heap = HeapContaining(g_allHeaps, address);
    uint32_t protect = heap != nullptr ? heap->PageProtect(address) : 0;
    // The image and host-owned structures are not heap allocations but are always read/write.
    const bool hostRegion = address >= HostRegionBegin && address < HostRegionEnd;
    const bool image = address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    if (heap == nullptr && (hostRegion || image))
        protect = 0x04; // PAGE_READWRITE

    if (ShouldLog(calls))
        LogPrint("[mem] MmQueryAddressProtect(%08X) -> %X", address, protect);
    ctx.r3.u64 = protect;
}

// u32 MmGetPhysicalAddress(u32 address)
// Physical windows map onto the low 512 MB. Xenia also adds 0x1000 for the 0xE0000000 window; not done
// here until something (the GPU side) depends on it.
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    static std::atomic<uint32_t> calls;
    const uint32_t address = ctx.r3.u32;
    const uint32_t physical = address & 0x1FFFFFFF;

    if (ShouldLog(calls))
        LogPrint("[mem] MmGetPhysicalAddress(%08X) -> %08X", address, physical);
    ctx.r3.u64 = physical;
}
