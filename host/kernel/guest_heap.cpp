#include "guest_heap.h"
#include "xbox.h"
#include "memory.h"

#include <algorithm>
#include <cstring>

GuestHeap::GuestHeap(const char* name, uint32_t begin, uint64_t end, uint32_t pageSize)
    : m_name(name), m_begin(begin), m_end(end), m_pageSize(pageSize), m_pages(size_t((end - begin) / pageSize))
{
}

bool GuestHeap::RangeFree(uint32_t first, uint32_t count) const
{
    return FirstUsed(first, count) == UINT32_MAX;
}

uint32_t GuestHeap::FirstUsed(uint32_t first, uint32_t count) const
{
    for (uint32_t i = first; i < first + count; i++)
    {
        if (m_pages[i].state != 0)
            return i;
    }
    return UINT32_MAX;
}

uint32_t GuestHeap::LastUsed(uint32_t first, uint32_t count) const
{
    for (uint32_t i = first + count; i > first; i--)
    {
        if (m_pages[i - 1].state != 0)
            return i - 1;
    }
    return UINT32_MAX;
}

void GuestHeap::MarkAllocated(uint32_t first, uint32_t count, bool commit, uint32_t protect)
{
    for (uint32_t i = first; i < first + count; i++)
    {
        Page& page = m_pages[i];
        page.state = commit ? X_MEM_COMMIT : X_MEM_RESERVE;
        page.protect = commit ? protect : 0;
        page.allocationBase = first;
        page.allocationProtect = protect;
    }
    m_pages[first].allocationPages = count;
}

bool GuestHeap::RangeInOneAllocation(uint32_t first, uint32_t count) const
{
    if (m_pages[first].state == 0)
        return false;

    const Page& base = m_pages[m_pages[first].allocationBase];
    return first + count <= m_pages[first].allocationBase + base.allocationPages;
}

void GuestHeap::Zero(uint32_t first, uint32_t count)
{
    std::memset(g_base + PageAddress(first), 0, size_t(count) * m_pageSize);
}

uint32_t GuestHeap::Allocate(uint32_t size, uint32_t alignment, bool commit, bool topDown, uint32_t protect,
    uint32_t low, uint64_t high)
{
    if (size == 0)
        return 0;

    const uint32_t count = uint32_t((uint64_t(size) + m_pageSize - 1) / m_pageSize);
    const uint32_t step = std::max<uint32_t>(1, alignment / m_pageSize);

    // Candidate page range, with first pages rounded up to the alignment (relative to guest address 0).
    const uint64_t lowAddress = std::max<uint64_t>(low, m_begin);
    const uint64_t highAddress = std::min<uint64_t>(high, m_end);
    if (highAddress <= lowAddress || highAddress - lowAddress < uint64_t(count) * m_pageSize)
        return 0;

    const uint64_t alignBytes = uint64_t(step) * m_pageSize;
    const uint64_t firstCandidate = (lowAddress + alignBytes - 1) / alignBytes * alignBytes;
    const uint64_t lastCandidate = (highAddress - uint64_t(count) * m_pageSize) / alignBytes * alignBytes;
    if (lastCandidate < firstCandidate)
        return 0;

    // On a collision, jump the candidate past the page in use instead of trying every position.
    std::lock_guard lock(m_mutex);
    const uint64_t span = uint64_t(count) * m_pageSize;
    uint64_t address = topDown ? lastCandidate : firstCandidate;
    while (address >= firstCandidate && address <= lastCandidate)
    {
        const uint32_t first = PageIndex(uint32_t(address));
        const uint32_t used = topDown ? LastUsed(first, count) : FirstUsed(first, count);
        if (used == UINT32_MAX)
        {
            MarkAllocated(first, count, commit, protect);
            return uint32_t(address);
        }

        if (topDown)
        {
            const uint64_t usedAddress = PageAddress(used);
            if (usedAddress < span)
                break;
            address = (usedAddress - span) / alignBytes * alignBytes;
        }
        else
        {
            address = (uint64_t(PageAddress(used)) + m_pageSize + alignBytes - 1) / alignBytes * alignBytes;
        }
    }
    return 0;
}

bool GuestHeap::AllocateFixed(uint32_t address, uint32_t size, bool commit, uint32_t protect)
{
    if (size == 0 || address % m_pageSize != 0 || !Contains(address) || uint64_t(address) + size > m_end)
        return false;

    const uint32_t first = PageIndex(address);
    const uint32_t count = uint32_t((uint64_t(size) + m_pageSize - 1) / m_pageSize);

    std::lock_guard lock(m_mutex);
    if (!RangeFree(first, count))
        return false;

    MarkAllocated(first, count, commit, protect);
    return true;
}

bool GuestHeap::Commit(uint32_t address, uint32_t size, uint32_t protect)
{
    if (size == 0 || !Contains(address) || uint64_t(address) + size > m_end)
        return false;

    const uint32_t first = PageIndex(address);
    const uint32_t count = uint32_t((uint64_t(address % m_pageSize) + size + m_pageSize - 1) / m_pageSize);

    std::lock_guard lock(m_mutex);
    if (!RangeInOneAllocation(first, count))
        return false;

    for (uint32_t i = first; i < first + count; i++)
    {
        m_pages[i].state = X_MEM_COMMIT;
        m_pages[i].protect = protect;
    }
    return true;
}

bool GuestHeap::Decommit(uint32_t address, uint32_t size)
{
    if (size == 0 || !Contains(address) || uint64_t(address) + size > m_end)
        return false;

    const uint32_t first = PageIndex(address);
    const uint32_t count = uint32_t((uint64_t(address % m_pageSize) + size + m_pageSize - 1) / m_pageSize);

    std::lock_guard lock(m_mutex);
    if (!RangeInOneAllocation(first, count))
        return false;

    for (uint32_t i = first; i < first + count; i++)
    {
        m_pages[i].state = X_MEM_RESERVE;
        m_pages[i].protect = 0;
    }
    Zero(first, count);
    return true;
}

uint32_t GuestHeap::Release(uint32_t address)
{
    if (!Contains(address) || address % m_pageSize != 0)
        return 0;

    const uint32_t first = PageIndex(address);

    std::lock_guard lock(m_mutex);
    if (m_pages[first].state == 0 || m_pages[first].allocationBase != first)
        return 0;

    const uint32_t count = m_pages[first].allocationPages;
    Zero(first, count);
    std::fill(m_pages.begin() + first, m_pages.begin() + first + count, Page{});
    return count * m_pageSize;
}

bool GuestHeap::Query(uint32_t address, Info& info) const
{
    if (!Contains(address))
        return false;

    std::lock_guard lock(m_mutex);
    const uint32_t first = PageIndex(address);
    const Page& page = m_pages[first];

    // Extend over following pages with the same state and protection, within the same allocation.
    uint32_t end = first + 1;
    const uint32_t limit = page.state != 0 ? page.allocationBase + m_pages[page.allocationBase].allocationPages : uint32_t(m_pages.size());
    while (end < limit && m_pages[end].state == page.state && m_pages[end].protect == page.protect)
        end++;

    info.baseAddress = PageAddress(first);
    info.regionSize = (end - first) * m_pageSize;
    if (page.state == 0)
    {
        info.allocationBase = 0;
        info.allocationProtect = 0;
        info.state = X_MEM_FREE;
        info.protect = 0;
        info.type = 0;
    }
    else
    {
        info.allocationBase = PageAddress(page.allocationBase);
        info.allocationProtect = page.allocationProtect;
        info.state = page.state;
        info.protect = page.protect;
        info.type = X_MEM_PRIVATE;
    }
    return true;
}

uint32_t GuestHeap::AllocationBase(uint32_t address) const
{
    if (!Contains(address))
        return 0;

    std::lock_guard lock(m_mutex);
    const Page& page = m_pages[PageIndex(address)];
    return page.state != 0 ? PageAddress(page.allocationBase) : 0;
}

uint32_t GuestHeap::AllocationSize(uint32_t address) const
{
    if (!Contains(address))
        return 0;

    std::lock_guard lock(m_mutex);
    const Page& page = m_pages[PageIndex(address)];
    return page.state != 0 ? m_pages[page.allocationBase].allocationPages * m_pageSize : 0;
}

uint32_t GuestHeap::PageProtect(uint32_t address) const
{
    if (!Contains(address))
        return 0;

    std::lock_guard lock(m_mutex);
    const Page& page = m_pages[PageIndex(address)];
    return page.state == X_MEM_COMMIT ? page.protect : 0;
}
