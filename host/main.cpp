#include "loader.h"
#include "log.h"
#include "memory.h"
#include "stubs.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <cinttypes>
#include <cstdlib>
#include <new>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

// Host stack for the guest main thread: every guest call is a host call, so it needs room.
constexpr size_t GuestThreadHostStackSize = 64 * 1024 * 1024;
constexpr uint32_t DefaultGuestStackSize = 256 * 1024;

// Xbox 360 processor control region fields the CRT reads through r13 (layout as in Xenia's X_KPCR).
constexpr uint32_t PcrTlsPtr = 0x000;
constexpr uint32_t PcrSelfPtr = 0x030;
constexpr uint32_t PcrStackBase = 0x070; // high end of the stack
constexpr uint32_t PcrStackEnd = 0x074;  // low end of the stack
constexpr uint32_t PcrCurrentThread = 0x100;

static thread_local PPCContext* g_currentContext;
static XexInfo g_xex;

// Every table slot without a recompiled function points here, so a bad indirect call is reported.
static PPC_FUNC(HostUnmappedCall)
{
    LogPrint("fatal: indirect call to a guest address with no recompiled function (ctr=%08X lr=%08X)",
        ctx.ctr.u32, uint32_t(ctx.lr));
    StubPrintSummary();
    LogFlush();
    std::_Exit(5);
}

static void InitFunctionTable()
{
    for (uint32_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
        PPC_LOOKUP_FUNC(g_base, address) = HostUnmappedCall;

    size_t count = 0;
    for (PPCFuncMapping* mapping = PPCFuncMappings; mapping->host != nullptr; mapping++, count++)
        PPC_LOOKUP_FUNC(g_base, mapping->guest) = mapping->host;

    LogPrint("function table: %zu functions", count);
}

static void LogHostLocation(const char* label, uint64_t address)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 256;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
        LogPrint("  %s: %s+0x%" PRIX64, label, symbol->Name, uint64_t(displacement));
    else
        LogPrint("  %s: host 0x%" PRIX64 " (no symbol)", label, address);
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_BREAKPOINT && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    LogPrint("fatal: host exception 0x%08lX", code);
    LogHostLocation("in", info->ContextRecord->Rip);
    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        const auto* fault = reinterpret_cast<const uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
        const char* kind = info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "read";
        if (IsGuestHostPointer(fault))
            LogPrint("  %s of guest address 0x%08X", kind, uint32_t(fault - g_base));
        else
            LogPrint("  %s of host address %p", kind, static_cast<const void*>(fault));
    }

    if (PPCContext* ctx = g_currentContext)
        LogPrint("  guest r1=%08X r3=%08X r4=%08X r5=%08X lr=%08X ctr=%08X",
            ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, uint32_t(ctx->lr), ctx->ctr.u32);

    const char* lastStub = StubLastCalled();
    LogPrint("  last import stub called: %s", lastStub != nullptr ? lastStub : "(none)");
    StubPrintSummary();
    LogFlush();
    TerminateProcess(GetCurrentProcess(), 6);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI GuestMainThread(void*)
{
    const uint32_t stackSize = g_xex.stackSize != 0 ? g_xex.stackSize : DefaultGuestStackSize;
    const uint32_t stackLow = GuestAlloc(stackSize, 0x10000);
    const uint32_t stackHigh = stackLow + stackSize;

    // Only the PCR fields above are filled in; the thread object and TLS slots are zeroed placeholders.
    const uint32_t pcr = GuestAlloc(0x1000, 0x1000);
    const uint32_t thread = GuestAlloc(0x1000, 0x1000);
    const uint32_t tls = GuestAlloc(0x1000, 0x1000);
    GuestStoreU32(pcr + PcrTlsPtr, tls);
    GuestStoreU32(pcr + PcrSelfPtr, pcr);
    GuestStoreU32(pcr + PcrStackBase, stackHigh);
    GuestStoreU32(pcr + PcrStackEnd, stackLow);
    GuestStoreU32(pcr + PcrCurrentThread, thread);

    auto* ctx = new PPCContext{};
    ctx->r1.u64 = stackHigh - 0x100;
    ctx->r13.u64 = pcr;
    ctx->fpscr.loadFromHost();
    g_currentContext = ctx;

    LogPrint("guest main thread: stack 0x%08X-0x%08X, pcr 0x%08X; calling entry point 0x%08X",
        stackLow, stackHigh, pcr, g_xex.entryPoint);
    LogFlush();

    PPCFunc* entry = PPC_LOOKUP_FUNC(g_base, g_xex.entryPoint);
    entry(*ctx, g_base);

    LogPrint("entry point returned (r3=%08X)", ctx->r3.u32);
    return 0;
}

int main(int argc, char** argv)
{
    const char* xexPath = argc > 1 ? argv[1] : "private\\default.xex";
    LogInit("host_log.txt");

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, CrashHandler);

    if (!GuestMemoryInit() || !LoadXex(xexPath, g_xex))
        return 1;
    InitFunctionTable();

    HANDLE thread = CreateThread(nullptr, GuestThreadHostStackSize, GuestMainThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr)
    {
        LogPrint("error: could not create the guest main thread (error %lu)", GetLastError());
        return 1;
    }
    WaitForSingleObject(thread, INFINITE);

    StubPrintSummary();
    LogFlush();
    return 0;
}
