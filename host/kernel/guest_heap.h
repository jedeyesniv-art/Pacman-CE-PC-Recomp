#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

// Page-granular bookkeeping for one guest address range. All guest memory is already committed by the
// host, so this only tracks which pages are free, reserved or committed. Memory is zeroed when it is
// decommitted or released, so every new allocation reads as zero like fresh pages do.
class GuestHeap
{
public:
    GuestHeap(const char* name, uint32_t begin, uint64_t end, uint32_t pageSize);

    const char* Name() const { return m_name; }
    uint32_t Begin() const { return m_begin; }
    uint64_t End() const { return m_end; }
    uint32_t PageSize() const { return m_pageSize; }
    bool Contains(uint32_t address) const { return address >= m_begin && address < m_end; }

    // Reserves (and optionally commits) size bytes with the given alignment inside [low, high)
    // (clamped to the heap). Returns the guest address, or 0 if there is no room.
    uint32_t Allocate(uint32_t size, uint32_t alignment, bool commit, bool topDown, uint32_t protect,
        uint32_t low = 0, uint64_t high = UINT64_MAX);

    // Reserves at a fixed page-aligned address. Fails if any page in the range is in use.
    bool AllocateFixed(uint32_t address, uint32_t size, bool commit, uint32_t protect);

    // Commits pages that are reserved; the range must lie inside one allocation.
    bool Commit(uint32_t address, uint32_t size, uint32_t protect);

    // Decommits committed or reserved pages back to reserved, zeroing them.
    bool Decommit(uint32_t address, uint32_t size);

    // Releases the whole allocation whose base is address, zeroing it. Returns its size, 0 on failure.
    uint32_t Release(uint32_t address);

    struct Info
    {
        uint32_t baseAddress;
        uint32_t allocationBase;
        uint32_t allocationProtect;
        uint32_t regionSize;
        uint32_t state;   // X_MEM_COMMIT, X_MEM_RESERVE or X_MEM_FREE
        uint32_t protect;
        uint32_t type;    // X_MEM_PRIVATE, 0 if free
    };

    // Describes the run of pages from address's page that share the same state, like VirtualQuery.
    bool Query(uint32_t address, Info& info) const;

    // Start and size of the allocation containing address; 0 if it is free.
    uint32_t AllocationBase(uint32_t address) const;
    uint32_t AllocationSize(uint32_t address) const;

    // Protection of address's page if committed, else 0.
    uint32_t PageProtect(uint32_t address) const;

private:
    struct Page
    {
        uint32_t state = 0;           // X_MEM_FREE, X_MEM_RESERVE or X_MEM_COMMIT (0 = free)
        uint32_t protect = 0;
        uint32_t allocationBase = 0;  // page index where the allocation starts
        uint32_t allocationPages = 0; // valid on the allocation's first page
        uint32_t allocationProtect = 0;
    };

    uint32_t PageIndex(uint32_t address) const { return (address - m_begin) / m_pageSize; }
    uint32_t PageAddress(uint32_t index) const { return m_begin + index * m_pageSize; }
    bool RangeFree(uint32_t first, uint32_t count) const;
    // Index of the first / last non-free page in the range, UINT32_MAX if all are free.
    uint32_t FirstUsed(uint32_t first, uint32_t count) const;
    uint32_t LastUsed(uint32_t first, uint32_t count) const;
    void MarkAllocated(uint32_t first, uint32_t count, bool commit, uint32_t protect);
    bool RangeInOneAllocation(uint32_t first, uint32_t count) const;
    void Zero(uint32_t first, uint32_t count);

    const char* m_name;
    uint32_t m_begin;
    uint64_t m_end;
    uint32_t m_pageSize;
    std::vector<Page> m_pages;
    mutable std::mutex m_mutex;
};
