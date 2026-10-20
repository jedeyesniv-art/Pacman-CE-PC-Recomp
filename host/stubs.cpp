#include "stubs.h"
#include "log.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

// Calls logged per import before it goes quiet; the exit summary still counts all of them.
constexpr uint32_t LoggedCallsPerStub = 8;

static std::mutex g_registryMutex;
static StubInfo* g_registry;
static std::atomic<const char*> g_lastCalled;

static void LogCall(StubInfo& info, PPCContext& ctx, uint32_t call)
{
    LogPrint("[stub] %s #%u r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X lr=%08X",
        info.name, call, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr));
}

static uint32_t Record(StubInfo& info)
{
    g_lastCalled.store(info.name, std::memory_order_relaxed);
    uint32_t call = info.calls.fetch_add(1) + 1;
    if (call == 1)
    {
        std::lock_guard lock(g_registryMutex);
        info.next = g_registry;
        g_registry = &info;
    }
    return call;
}

void StubCalled(StubInfo& info, PPCContext& ctx)
{
    uint32_t call = Record(info);
    if (call <= LoggedCallsPerStub)
        LogCall(info, ctx, call);
    if (call == LoggedCallsPerStub)
        LogPrint("[stub] %s: further calls not logged", info.name);
}

void StubFatal(StubInfo& info, PPCContext& ctx)
{
    uint32_t call = Record(info);
    LogCall(info, ctx, call);
    LogPrint("fatal: %s does not return; stopping", info.name);
    StubPrintSummary();
    LogFlush();
    std::_Exit(3);
}

const char* StubLastCalled()
{
    return g_lastCalled.load(std::memory_order_relaxed);
}

void StubPrintSummary()
{
    std::vector<StubInfo*> called;
    {
        std::lock_guard lock(g_registryMutex);
        for (StubInfo* info = g_registry; info != nullptr; info = info->next)
            called.push_back(info);
    }
    std::sort(called.begin(), called.end(), [](StubInfo* a, StubInfo* b) { return a->calls > b->calls; });

    LogPrint("stub summary: %zu distinct imports called", called.size());
    for (StubInfo* info : called)
        LogPrint("  %8u  %s", info->calls.load(), info->name);
}
