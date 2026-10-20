// Threads and time: ExCreateThread / ExTerminateThread, resume, affinity and priority, sleeping,
// KeQueryPerformanceFrequency / KeQuerySystemTime. Each guest thread runs on its own host thread.
// Argument order follows Xenia (xboxkrnl_threading.cc) and the game's CreateThread wrapper (sub_820CA7A0).
#include "dispatcher.h"
#include "handles.h"
#include "xbox.h"
#include "guest_thread.h"
#include "log.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

constexpr size_t HostThreadStackSize = 64 * 1024 * 1024; // every guest call is a host call
constexpr uint32_t DefaultGuestStackSize = 0x10000;
constexpr uint32_t CreateSuspended = 0x1; // ExCreateThread flags bit 0
constexpr uint32_t LoggedCalls = 32;

// XenonRecomp turns mftb (the guest timebase) into __rdtsc(), so the frequency reported to the guest must
// be the host TSC's, not the Xbox's 50 MHz. Measured once against QueryPerformanceCounter.
static uint64_t TimebaseFrequency()
{
    static const uint64_t frequency = []
    {
        LARGE_INTEGER qpcFrequency, qpcStart, qpcNow;
        QueryPerformanceFrequency(&qpcFrequency);
        QueryPerformanceCounter(&qpcStart);
        const uint64_t tscStart = __rdtsc();
        do
            QueryPerformanceCounter(&qpcNow);
        while (qpcNow.QuadPart - qpcStart.QuadPart < qpcFrequency.QuadPart / 20); // 50 ms
        const uint64_t tscTicks = __rdtsc() - tscStart;
        const uint64_t result = tscTicks * qpcFrequency.QuadPart / (qpcNow.QuadPart - qpcStart.QuadPart);
        LogPrint("[time] timebase (host TSC) frequency %llu Hz", static_cast<unsigned long long>(result));
        return result;
    }();
    return frequency;
}

struct HostThread
{
    GuestThread guest;
    uint32_t startup;
    uint32_t start;
    uint32_t context;
    uint32_t affinity;
    int32_t priority = 0;

    std::mutex mutex;
    std::condition_variable resumed;
    uint32_t suspendCount;
};

static std::mutex g_threadsMutex;
static std::unordered_map<uint32_t, HostThread*> g_threads; // by KTHREAD address

static HostThread* FindThread(uint32_t threadObject)
{
    std::lock_guard lock(g_threadsMutex);
    auto it = g_threads.find(threadObject);
    return it != g_threads.end() ? it->second : nullptr;
}

static bool ShouldLog(std::atomic<uint32_t>& calls)
{
    return calls.fetch_add(1) < LoggedCalls;
}

static DWORD WINAPI HostThreadMain(void* parameter)
{
    auto* thread = static_cast<HostThread*>(parameter);
    ULONG guarantee = 256 * 1024; // room for the crash handler after a host stack overflow
    SetThreadStackGuarantee(&guarantee);

    {
        std::unique_lock lock(thread->mutex);
        thread->resumed.wait(lock, [&] { return thread->suspendCount == 0; });
    }

    uint32_t exitCode;
    try
    {
        // XapiThreadStartup(start, context) calls start(context) and then ExTerminateThread.
        exitCode = thread->startup != 0
            ? CallGuest(thread->guest, thread->startup, { thread->start, thread->context })
            : CallGuest(thread->guest, thread->start, { thread->context });
    }
    catch (const GuestThreadExit& exit)
    {
        exitCode = exit.exitCode;
    }

    LogPrint("[thread] thread %u ended (exit code %08X)", thread->guest.id, exitCode);
    DispatcherSetState(thread->guest.threadObject, 1);
    return 0;
}

static uint32_t Resume(HostThread* thread)
{
    uint32_t previous;
    {
        std::lock_guard lock(thread->mutex);
        previous = thread->suspendCount;
        if (thread->suspendCount != 0)
            thread->suspendCount--;
    }
    thread->resumed.notify_all();
    return previous;
}

// NTSTATUS ExCreateThread(u32* handle, u32 stackSize, u32* threadId, u32 xapiStartup, u32 start,
//     u32 context, u32 flags). flags bit 0: start suspended; bits 24-31: CPU affinity mask.
PPC_FUNC(__imp__ExCreateThread)
{
    const uint32_t handlePtr = ctx.r3.u32, stackSize = ctx.r4.u32, idPtr = ctx.r5.u32;
    const uint32_t startup = ctx.r6.u32, start = ctx.r7.u32, context = ctx.r8.u32, flags = ctx.r9.u32;

    const uint32_t affinity = flags >> 24;
    uint8_t cpu = 0;
    if (affinity != 0)
        cpu = uint8_t(__builtin_ctz(affinity));

    auto* thread = new HostThread{};
    thread->guest = CreateGuestThread(stackSize != 0 ? (stackSize + 0xFFFF) & ~0xFFFFu : DefaultGuestStackSize, cpu, false);
    thread->startup = startup;
    thread->start = start;
    thread->context = context;
    thread->affinity = affinity != 0 ? affinity : 1;
    thread->suspendCount = (flags & CreateSuspended) ? 1 : 0;
    {
        std::lock_guard lock(g_threadsMutex);
        g_threads.emplace(thread->guest.threadObject, thread);
    }

    const uint32_t handle = HandleCreate(thread->guest.threadObject);
    if (handlePtr != 0)
        GuestStoreU32(handlePtr, handle);
    if (idPtr != 0)
        GuestStoreU32(idPtr, thread->guest.id);

    LogPrint("[thread] ExCreateThread(start=%08X context=%08X startup=%08X stack=%X flags=%08X) -> thread %u handle %08X%s",
        start, context, startup, stackSize, flags, thread->guest.id, handle, thread->suspendCount ? " (suspended)" : "");

    HANDLE host = CreateThread(nullptr, HostThreadStackSize, HostThreadMain, thread, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (host == nullptr)
    {
        LogPrint("[thread] error: could not create a host thread (error %lu)", GetLastError());
        ctx.r3.u64 = STATUS_NO_MEMORY;
        return;
    }
    CloseHandle(host);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// void ExTerminateThread(u32 exitCode): never returns.
PPC_FUNC(__imp__ExTerminateThread)
{
    throw GuestThreadExit{ ctx.r3.u32 };
}

// u32 KeResumeThread(KTHREAD* thread): previous suspend count.
PPC_FUNC(__imp__KeResumeThread)
{
    HostThread* thread = FindThread(ctx.r3.u32);
    ctx.r3.u64 = thread != nullptr ? Resume(thread) : 0;
}

// NTSTATUS NtResumeThread(u32 handle, u32* previousSuspendCount)
PPC_FUNC(__imp__NtResumeThread)
{
    const uint32_t handle = ctx.r3.u32, previousPtr = ctx.r4.u32;
    HostThread* thread = FindThread(HandleObject(handle));
    if (thread == nullptr)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }

    const uint32_t previous = Resume(thread);
    if (previousPtr != 0)
        GuestStoreU32(previousPtr, previous);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS KeSetAffinityThread(KTHREAD* thread, u32 affinity, u32* previousAffinity)
// The Xbox returns the previous affinity through the pointer, not as the result (per Xenia).
// Affinity is recorded but not applied to the host thread.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    static std::atomic<uint32_t> calls;
    const uint32_t threadObject = ctx.r3.u32, affinity = ctx.r4.u32, previousPtr = ctx.r5.u32;
    HostThread* thread = FindThread(threadObject);
    const uint32_t previous = thread != nullptr ? thread->affinity : 1;
    if (thread != nullptr)
        thread->affinity = affinity;
    if (previousPtr != 0)
        GuestStoreU32(previousPtr, previous);

    if (ShouldLog(calls))
        LogPrint("[thread] KeSetAffinityThread(%08X, %X) previous %X", threadObject, affinity, previous);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// s32 KeSetBasePriorityThread(KTHREAD* thread, s32 increment): previous priority. Recorded, not applied.
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    static std::atomic<uint32_t> calls;
    const uint32_t threadObject = ctx.r3.u32;
    const int32_t priority = ctx.r4.s32;
    HostThread* thread = FindThread(threadObject);
    const int32_t previous = thread != nullptr ? thread->priority : 0;
    if (thread != nullptr)
        thread->priority = priority;

    if (ShouldLog(calls))
        LogPrint("[thread] KeSetBasePriorityThread(%08X, %d) previous %d", threadObject, priority, previous);
    ctx.r3.u64 = uint32_t(previous);
}

// NTSTATUS KeDelayExecutionThread(u32 processorMode, u32 alertable, LARGE_INTEGER* interval)
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    const Deadline deadline = DeadlineFromTimeout(ctx.r5.u32);
    if (!deadline || *deadline <= std::chrono::steady_clock::now())
        SwitchToThread();
    else
        std::this_thread::sleep_until(*deadline);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// u64 KeQueryPerformanceFrequency()
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    ctx.r3.u64 = TimebaseFrequency();
}

// void KeQuerySystemTime(u64* time): 100 ns units since 1601.
PPC_FUNC(__imp__KeQuerySystemTime)
{
    const uint64_t time = SystemTime100ns();
    GuestStoreU32(ctx.r3.u32, uint32_t(time >> 32));
    GuestStoreU32(ctx.r3.u32 + 4, uint32_t(time));
}
