#pragma once

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <cstdint>

// Per-import call record for an unimplemented import stub.
struct StubInfo
{
    const char* name;
    std::atomic<uint32_t> calls{ 0 };
    StubInfo* next = nullptr; // registration list for the exit summary
};

// Logs the call (the first few per import) and returns; the stub then returns 0 in r3.
void StubCalled(StubInfo& info, PPCContext& ctx);

// Logs the call and stops the host: for imports that never return on real hardware.
[[noreturn]] void StubFatal(StubInfo& info, PPCContext& ctx);

// Name of the most recently called stub, for crash reports. Null if none yet.
const char* StubLastCalled();

// Prints call counts for every stub that was called.
void StubPrintSummary();

#define HOST_STUB(name) \
    __attribute__((weak)) PPC_FUNC(__imp__##name) \
    { \
        static StubInfo info{ #name }; \
        StubCalled(info, ctx); \
        ctx.r3.u64 = 0; \
    }

#define HOST_FATAL_STUB(name) \
    __attribute__((weak)) PPC_FUNC(__imp__##name) \
    { \
        static StubInfo info{ #name }; \
        StubFatal(info, ctx); \
    }
