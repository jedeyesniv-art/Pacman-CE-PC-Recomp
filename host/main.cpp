#include "guest_thread.h"
#include "loader.h"
#include "log.h"
#include "memory.h"
#include "stubs.h"
#include "kernel/tls_imports.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <new>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

// Host stack for the guest main thread: every guest call is a host call, so it needs room.
constexpr size_t GuestThreadHostStackSize = 64 * 1024 * 1024;
constexpr uint32_t DefaultGuestStackSize = 256 * 1024;

// If nothing is logged for this long, the hang reporter prints where the guest main thread is.
constexpr DWORD QuietReportMilliseconds = 10000;

static PPCContext* g_mainContext; // the guest main thread's context, for the hang reporter
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

// Unwinds from the faulting context so the report shows the chain of recompiled functions.
static void LogBacktrace(const CONTEXT& faultContext)
{
    constexpr int MaxFrames = 24;
    CONTEXT context = faultContext;
    LogPrint("  backtrace:");
    for (int frame = 0; frame < MaxFrames && context.Rip != 0; frame++)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "#%d", frame);
        LogHostLocation(label, context.Rip);

        DWORD64 imageBase = 0;
        RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function == nullptr)
            break; // leaf or unknown code: stop rather than guess

        void* handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
    }
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
            LogPrint("  %s of guest address 0x%08X%s", kind, uint32_t(fault - g_base),
                IsGuestStackGuard(uint32_t(fault - g_base)) ? " - guest stack overflow (guard region)" : "");
        else
            LogPrint("  %s of host address %p", kind, static_cast<const void*>(fault));
    }

    if (PPCContext* ctx = CurrentGuestContext())
        LogPrint("  guest r1=%08X r3=%08X r4=%08X r5=%08X lr=%08X ctr=%08X",
            ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, uint32_t(ctx->lr), ctx->ctr.u32);

    const char* lastStub = StubLastCalled();
    LogPrint("  last import stub called: %s", lastStub != nullptr ? lastStub : "(none)");
    LogBacktrace(*info->ContextRecord);
    StubPrintSummary();
    LogFlush();
    TerminateProcess(GetCurrentProcess(), 6);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI GuestMainThread(void*)
{
    // Keep stack for the crash handler when the guest overflows the host stack.
    ULONG guarantee = 256 * 1024;
    SetThreadStackGuarantee(&guarantee);

    const uint32_t stackSize = g_xex.stackSize != 0 ? g_xex.stackSize : DefaultGuestStackSize;
    GuestThread thread = CreateGuestThread(stackSize, 0, false);
    g_mainContext = thread.ctx;

    LogPrint("guest main thread: stack 0x%08X-0x%08X, pcr 0x%08X; calling entry point 0x%08X",
        thread.stackLow, thread.stackHigh, thread.pcr, g_xex.entryPoint);
    LogFlush();

    const uint32_t result = CallGuest(thread, g_xex.entryPoint, {});
    LogPrint("entry point returned (r3=%08X)", result);
    return 0;
}

// Briefly suspends the guest main thread and logs where it is, then lets it continue.
// Diagnostic only: if the thread were suspended holding a host lock (log, heap) this could deadlock;
// it only runs after a quiet period, when the guest is not logging.
static void ReportQuietThread(HANDLE thread)
{
    if (SuspendThread(thread) == DWORD(-1))
        return;

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context))
    {
        LogPrint("no log output for %lu s - guest main thread is at:", QuietReportMilliseconds / 1000);
        if (PPCContext* ctx = g_mainContext)
            LogPrint("  guest r1=%08X r3=%08X r4=%08X r5=%08X lr=%08X ctr=%08X",
                ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, uint32_t(ctx->lr), ctx->ctr.u32);
        const char* lastStub = StubLastCalled();
        LogPrint("  last import stub called: %s", lastStub != nullptr ? lastStub : "(none)");
        LogBacktrace(context);
        LogFlush();
    }
    ResumeThread(thread);
}

// Waits for the guest main thread, reporting once per quiet period where it is stuck.
static void WaitForGuest(HANDLE thread)
{
    unsigned long long lastLines = LogLineCount();
    DWORD quietFor = 0;
    bool reported = false;
    while (WaitForSingleObject(thread, 1000) == WAIT_TIMEOUT)
    {
        const unsigned long long lines = LogLineCount();
        if (lines != lastLines)
        {
            lastLines = lines;
            quietFor = 0;
            reported = false;
            continue;
        }

        quietFor += 1000;
        if (quietFor >= QuietReportMilliseconds && !reported)
        {
            ReportQuietThread(thread);
            lastLines = LogLineCount();
            reported = true;
        }
    }
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
    TlsInit(g_xex.tlsSlots != 0 ? g_xex.tlsSlots : MaxTlsSlots);

    HANDLE thread = CreateThread(nullptr, GuestThreadHostStackSize, GuestMainThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr)
    {
        LogPrint("error: could not create the guest main thread (error %lu)", GetLastError());
        return 1;
    }
    WaitForGuest(thread);

    StubPrintSummary();
    LogFlush();
    return 0;
}
