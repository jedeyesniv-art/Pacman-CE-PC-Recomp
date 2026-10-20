#include "guest_thread.h"
#include "memory.h"
#include "kernel/dispatcher.h"

#include <atomic>

// Xbox 360 processor control region fields the game reads through r13 (layout as in Xenia's X_KPCR).
constexpr uint32_t PcrTlsPtr = 0x000;
constexpr uint32_t PcrSelfPtr = 0x030;
constexpr uint32_t PcrStackBase = 0x070; // high end of the stack
constexpr uint32_t PcrStackEnd = 0x074;  // low end of the stack
constexpr uint32_t PcrCurrentThread = 0x100;
constexpr uint32_t PcrCurrentCpu = 0x10C; // byte
constexpr uint32_t PcrDpcActive = 0x150;

// Space left above r1 at the top of the stack.
constexpr uint32_t StackTopReserve = 0x100;

static thread_local GuestThread* t_currentThread;
static std::atomic<uint32_t> g_nextThreadId{ 1 };

GuestThread CreateGuestThread(uint32_t stackSize, uint8_t cpu, bool dpcActive)
{
    GuestThread thread{};
    thread.id = g_nextThreadId.fetch_add(1);
    thread.stackLow = GuestAllocStack(stackSize);
    thread.stackHigh = thread.stackLow + stackSize;

    // Only these PCR fields are filled in; the thread object and TLS area are zeroed placeholders.
    // (Dynamic TLS slots live host-side, see kernel/tls_imports.cpp.)
    thread.pcr = GuestAlloc(0x1000, 0x1000);
    thread.threadObject = GuestAlloc(0x1000, 0x1000);
    const uint32_t tls = GuestAlloc(0x1000, 0x1000);
    GuestStoreU32(thread.pcr + PcrTlsPtr, tls);
    GuestStoreU32(thread.pcr + PcrSelfPtr, thread.pcr);
    GuestStoreU32(thread.pcr + PcrStackBase, thread.stackHigh);
    GuestStoreU32(thread.pcr + PcrStackEnd, thread.stackLow);
    GuestStoreU32(thread.pcr + PcrCurrentThread, thread.threadObject);
    g_base[thread.pcr + PcrCurrentCpu] = cpu;
    GuestStoreU32(thread.pcr + PcrDpcActive, dpcActive ? 1 : 0);
    DispatcherInit(thread.threadObject, DispatcherThread, 0); // waitable: signaled when the thread ends

    thread.ctx = new PPCContext{};
    thread.ctx->r13.u64 = thread.pcr;
    return thread;
}

uint32_t CallGuest(GuestThread& thread, uint32_t function, std::initializer_list<uint32_t> args)
{
    PPCContext& ctx = *thread.ctx;
    ctx.r1.u64 = thread.stackHigh - StackTopReserve;
    ctx.lr = 0;
    ctx.fpscr.loadFromHost();

    PPCRegister* const argRegs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
    size_t i = 0;
    for (uint32_t arg : args)
        argRegs[i++]->u64 = arg;

    // Restores the previous thread even when ExTerminateThread unwinds through here.
    struct Restore
    {
        GuestThread* previous;
        ~Restore() { t_currentThread = previous; }
    } restore{ t_currentThread };

    t_currentThread = &thread;
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, function);
    fn(ctx, g_base);
    return ctx.r3.u32;
}

PPCContext* CurrentGuestContext()
{
    return t_currentThread != nullptr ? t_currentThread->ctx : nullptr;
}

GuestThread* CurrentGuestThread()
{
    return t_currentThread;
}
