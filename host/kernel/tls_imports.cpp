// Dynamic TLS slots: KeTlsAlloc / KeTlsFree / KeTlsGetValue / KeTlsSetValue.
// Pac-Man's XEX TLS header has no static TLS data and no guest code reads the PCR's TLS pointer, so the
// slots are reachable only through these imports. Their values are kept host-side, one array per host
// thread; every guest thread runs on its own host thread.
#include "tls_imports.h"
#include "log.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <mutex>
#include <vector>

constexpr uint32_t TlsOutOfIndexes = 0xFFFFFFFF;
constexpr uint32_t LoggedCalls = 32;

struct ThreadTls;

static std::mutex g_tlsMutex;
static std::bitset<MaxTlsSlots> g_tlsUsed;
static uint32_t g_tlsSlotCount = MaxTlsSlots;
static std::vector<ThreadTls*> g_threads; // for KeTlsFree, which clears the slot in every thread

struct ThreadTls
{
    uint32_t values[MaxTlsSlots]{};

    ThreadTls()
    {
        std::lock_guard lock(g_tlsMutex);
        g_threads.push_back(this);
    }

    ~ThreadTls()
    {
        std::lock_guard lock(g_tlsMutex);
        g_threads.erase(std::remove(g_threads.begin(), g_threads.end(), this), g_threads.end());
    }
};

static thread_local ThreadTls t_tls;

static bool ShouldLog(std::atomic<uint32_t>& calls)
{
    return calls.fetch_add(1) < LoggedCalls;
}

void TlsInit(uint32_t slotCount)
{
    g_tlsSlotCount = std::min(slotCount, MaxTlsSlots);
    LogPrint("TLS: %u dynamic slots", g_tlsSlotCount);
}

// u32 KeTlsAlloc(): a free slot index, or 0xFFFFFFFF.
PPC_FUNC(__imp__KeTlsAlloc)
{
    static std::atomic<uint32_t> calls;
    uint32_t index = TlsOutOfIndexes;
    {
        std::lock_guard lock(g_tlsMutex);
        for (uint32_t i = 0; i < g_tlsSlotCount; i++)
        {
            if (!g_tlsUsed[i])
            {
                g_tlsUsed[i] = true;
                index = i;
                break;
            }
        }
    }

    if (ShouldLog(calls) || index == TlsOutOfIndexes)
        LogPrint("[tls] KeTlsAlloc() -> %08X", index);
    ctx.r3.u64 = index;
}

// BOOL KeTlsFree(u32 index): frees the slot and clears it in every thread.
PPC_FUNC(__imp__KeTlsFree)
{
    static std::atomic<uint32_t> calls;
    const uint32_t index = ctx.r3.u32;
    bool freed = false;
    {
        std::lock_guard lock(g_tlsMutex);
        if (index < g_tlsSlotCount && g_tlsUsed[index])
        {
            g_tlsUsed[index] = false;
            for (ThreadTls* thread : g_threads)
                thread->values[index] = 0;
            freed = true;
        }
    }

    if (ShouldLog(calls) || !freed)
        LogPrint("[tls] KeTlsFree(%u) -> %d", index, freed);
    ctx.r3.u64 = freed ? 1 : 0;
}

// u32 KeTlsGetValue(u32 index): this thread's value, 0 for an out-of-range index.
// Not logged: the CRT calls it constantly.
PPC_FUNC(__imp__KeTlsGetValue)
{
    const uint32_t index = ctx.r3.u32;
    ctx.r3.u64 = index < g_tlsSlotCount ? t_tls.values[index] : 0;
}

// BOOL KeTlsSetValue(u32 index, u32 value)
PPC_FUNC(__imp__KeTlsSetValue)
{
    static std::atomic<uint32_t> calls;
    const uint32_t index = ctx.r3.u32, value = ctx.r4.u32;
    const bool ok = index < g_tlsSlotCount;
    if (ok)
        t_tls.values[index] = value;

    if (ShouldLog(calls) || !ok)
        LogPrint("[tls] KeTlsSetValue(%u, %08X) -> %d", index, value, ok);
    ctx.r3.u64 = ok ? 1 : 0;
}
