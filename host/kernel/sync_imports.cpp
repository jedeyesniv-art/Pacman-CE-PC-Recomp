// Synchronisation imports: waits, events, semaphores, handles, critical sections, spinlocks and IRQL.
// Argument order and structure layouts follow Xenia (xboxkrnl_threading.cc, xboxkrnl_rtl.cc).
#include "dispatcher.h"
#include "handles.h"
#include "xbox.h"
#include "guest_thread.h"
#include "log.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

constexpr uint32_t WaitAll = 0; // KeWaitForMultipleObjects wait types
constexpr uint32_t SemaphoreLimitOffset = 0x10;
constexpr uint32_t LoggedCalls = 32;

static bool ShouldLog(std::atomic<uint32_t>& calls)
{
    return calls.fetch_add(1) < LoggedCalls;
}

// ---- Waits ----

// NTSTATUS KeWaitForSingleObject(void* object, u32 reason, u32 mode, u32 alertable, LARGE_INTEGER* timeout)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    const uint32_t object = ctx.r3.u32;
    ctx.r3.u64 = DispatcherWait(&object, 1, false, DeadlineFromTimeout(ctx.r7.u32));
}

// NTSTATUS KeWaitForMultipleObjects(u32 count, void** objects, u32 waitType, u32 reason, u32 mode,
//     u32 alertable, LARGE_INTEGER* timeout, void* waitBlocks)
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    const uint32_t count = ctx.r3.u32, array = ctx.r4.u32, waitType = ctx.r5.u32;
    std::vector<uint32_t> objects(count);
    for (uint32_t i = 0; i < count; i++)
        objects[i] = GuestLoadU32(array + i * 4);
    ctx.r3.u64 = DispatcherWait(objects.data(), count, waitType == WaitAll, DeadlineFromTimeout(ctx.r9.u32));
}

// NTSTATUS NtWaitForSingleObjectEx(u32 handle, u32 mode, u32 alertable, LARGE_INTEGER* timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    const uint32_t object = HandleObject(ctx.r3.u32);
    if (object == 0)
    {
        LogPrint("[sync] NtWaitForSingleObjectEx: invalid handle %08X", ctx.r3.u32);
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    ctx.r3.u64 = DispatcherWait(&object, 1, false, DeadlineFromTimeout(ctx.r6.u32));
}

// ---- Events and semaphores ----

// s32 KeSetEvent(KEVENT* event, s32 increment, u8 wait): previous state.
PPC_FUNC(__imp__KeSetEvent)
{
    ctx.r3.u64 = uint32_t(DispatcherSetState(ctx.r3.u32, 1));
}

// s32 KeResetEvent(KEVENT* event): previous state.
PPC_FUNC(__imp__KeResetEvent)
{
    ctx.r3.u64 = uint32_t(DispatcherSetState(ctx.r3.u32, 0));
}

// NTSTATUS NtCreateEvent(u32* handle, OBJECT_ATTRIBUTES* attributes, u32 eventType, u32 initialState)
// eventType 0 = notification (manual reset), 1 = synchronization (auto reset). The KEVENT lives in the
// host region so ObReferenceObjectByHandle can hand out a real guest pointer. Never freed.
PPC_FUNC(__imp__NtCreateEvent)
{
    static std::atomic<uint32_t> calls;
    const uint32_t handlePtr = ctx.r3.u32, attributes = ctx.r4.u32, type = ctx.r5.u32, initialState = ctx.r6.u32;

    const uint32_t event = GuestAlloc(DispatcherHeaderSize, 8);
    DispatcherInit(event, type == 1 ? DispatcherSynchronizationEvent : DispatcherNotificationEvent, initialState != 0 ? 1 : 0);
    const uint32_t handle = HandleCreate(event);
    GuestStoreU32(handlePtr, handle);

    if (ShouldLog(calls))
        LogPrint("[sync] NtCreateEvent(type=%u initial=%u attributes=%08X) -> handle %08X event %08X",
            type, initialState, attributes, handle, event);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtClearEvent(u32 handle)
PPC_FUNC(__imp__NtClearEvent)
{
    const uint32_t event = HandleObject(ctx.r3.u32);
    if (event == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    DispatcherSetState(event, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// void KeInitializeSemaphore(KSEMAPHORE* semaphore, s32 count, s32 limit)
PPC_FUNC(__imp__KeInitializeSemaphore)
{
    const uint32_t semaphore = ctx.r3.u32;
    DispatcherInit(semaphore, DispatcherSemaphore, ctx.r4.s32);
    GuestStoreU32(semaphore + SemaphoreLimitOffset, ctx.r5.u32);
}

// s32 KeReleaseSemaphore(KSEMAPHORE* semaphore, s32 increment, s32 adjustment, u8 wait): previous count.
PPC_FUNC(__imp__KeReleaseSemaphore)
{
    const uint32_t semaphore = ctx.r3.u32;
    const int32_t adjustment = ctx.r5.s32;
    const int32_t previous = DispatcherAddState(semaphore, adjustment);
    const int32_t limit = int32_t(GuestLoadU32(semaphore + SemaphoreLimitOffset));
    if (previous + adjustment > limit)
        LogPrint("[sync] KeReleaseSemaphore(%08X): count %d exceeds limit %d", semaphore, previous + adjustment, limit);
    ctx.r3.u64 = uint32_t(previous);
}

// ---- Handles and objects ----

// NTSTATUS NtClose(u32 handle)
PPC_FUNC(__imp__NtClose)
{
    ctx.r3.u64 = HandleClose(ctx.r3.u32) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

// NTSTATUS ObReferenceObjectByHandle(u32 handle, u32 objectType, void** object)
// Reference counts are not tracked; objects live until the host exits.
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    static std::atomic<uint32_t> calls;
    const uint32_t handle = ctx.r3.u32, objectPtr = ctx.r5.u32;
    const uint32_t object = HandleObject(handle);
    if (object != 0 && objectPtr != 0)
        GuestStoreU32(objectPtr, object);

    if (ShouldLog(calls) || object == 0)
        LogPrint("[sync] ObReferenceObjectByHandle(%08X, type %08X) -> %08X", handle, ctx.r4.u32, object);
    ctx.r3.u64 = object != 0 ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

// void ObDereferenceObject(void* object)
PPC_FUNC(__imp__ObDereferenceObject)
{
}

// ---- Critical sections ----
// X_RTL_CRITICAL_SECTION (28 bytes): dispatcher header, s32 lock count @0x10, s32 recursion count @0x14,
// u32 owning thread @0x18. The lock itself is host-side, keyed by the guest address; the guest fields are
// kept up to date for anything that inspects them.

constexpr uint32_t CsLockCount = 0x10;
constexpr uint32_t CsRecursionCount = 0x14;
constexpr uint32_t CsOwningThread = 0x18;

struct CriticalSection
{
    std::mutex mutex;
    std::condition_variable released;
    std::thread::id owner;
    uint32_t recursion = 0;
};

static std::mutex g_csMapMutex;
static std::unordered_map<uint32_t, std::unique_ptr<CriticalSection>> g_criticalSections;

static CriticalSection& GetCriticalSection(uint32_t address)
{
    std::lock_guard lock(g_csMapMutex);
    auto& entry = g_criticalSections[address];
    if (!entry)
        entry = std::make_unique<CriticalSection>(); // also covers sections the guest never initialised
    return *entry;
}

static uint32_t CurrentThreadId()
{
    const GuestThread* thread = CurrentGuestThread();
    return thread != nullptr ? thread->id : 0;
}

// void RtlInitializeCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    DispatcherInit(cs, DispatcherSynchronizationEvent, 0);
    GuestStoreU32(cs + CsLockCount, uint32_t(-1));
    GuestStoreU32(cs + CsRecursionCount, 0);
    GuestStoreU32(cs + CsOwningThread, 0);
    GetCriticalSection(cs);
}

// void RtlEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    const uint32_t address = ctx.r3.u32;
    CriticalSection& cs = GetCriticalSection(address);
    const std::thread::id self = std::this_thread::get_id();

    std::unique_lock lock(cs.mutex);
    cs.released.wait(lock, [&] { return cs.recursion == 0 || cs.owner == self; });
    cs.owner = self;
    cs.recursion++;
    GuestStoreU32(address + CsRecursionCount, cs.recursion);
    GuestStoreU32(address + CsOwningThread, CurrentThreadId());
}

// void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    const uint32_t address = ctx.r3.u32;
    CriticalSection& cs = GetCriticalSection(address);

    std::unique_lock lock(cs.mutex);
    if (cs.recursion == 0 || cs.owner != std::this_thread::get_id())
    {
        static std::atomic<uint32_t> calls;
        if (ShouldLog(calls))
            LogPrint("[sync] RtlLeaveCriticalSection(%08X) by a thread that does not own it", address);
        return;
    }

    cs.recursion--;
    GuestStoreU32(address + CsRecursionCount, cs.recursion);
    if (cs.recursion == 0)
    {
        cs.owner = {};
        GuestStoreU32(address + CsOwningThread, 0);
        lock.unlock();
        cs.released.notify_one();
    }
}

// ---- Spinlocks and IRQL ----
// A spinlock is a guest u32: 0 when free, otherwise the owner. IRQL is not modelled (always 0).

static void AcquireSpinLock(uint32_t address)
{
    std::atomic_ref<uint32_t> lock(*reinterpret_cast<uint32_t*>(g_base + address));
    const uint32_t owner = __builtin_bswap32(CurrentThreadId() | 0x80000000);
    uint32_t expected = 0;
    while (!lock.compare_exchange_weak(expected, owner, std::memory_order_acquire))
    {
        expected = 0;
        SwitchToThread();
    }
}

static void ReleaseSpinLock(uint32_t address)
{
    std::atomic_ref<uint32_t> lock(*reinterpret_cast<uint32_t*>(g_base + address));
    lock.store(0, std::memory_order_release);
}

// u32 KfAcquireSpinLock(u32* lock): previous IRQL.
PPC_FUNC(__imp__KfAcquireSpinLock)
{
    AcquireSpinLock(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// void KfReleaseSpinLock(u32* lock, u32 oldIrql)
PPC_FUNC(__imp__KfReleaseSpinLock)
{
    ReleaseSpinLock(ctx.r3.u32);
}

// void KeAcquireSpinLockAtRaisedIrql(u32* lock)
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    AcquireSpinLock(ctx.r3.u32);
}

// void KeReleaseSpinLockFromRaisedIrql(u32* lock)
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    ReleaseSpinLock(ctx.r3.u32);
}

// u32 KeRaiseIrqlToDpcLevel(): previous IRQL.
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)
{
    ctx.r3.u64 = 0;
}

// void KfLowerIrql(u32 irql)
PPC_FUNC(__imp__KfLowerIrql)
{
}

// void KeEnterCriticalRegion() / KeLeaveCriticalRegion(): disable/enable kernel APCs; nothing to do here.
PPC_FUNC(__imp__KeEnterCriticalRegion)
{
}

PPC_FUNC(__imp__KeLeaveCriticalRegion)
{
}
