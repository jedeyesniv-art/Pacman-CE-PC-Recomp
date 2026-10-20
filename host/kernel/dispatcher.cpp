#include "dispatcher.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

constexpr uint32_t HeaderType = 0;
constexpr uint32_t HeaderSize = 2;
constexpr uint32_t HeaderSignalState = 4;
constexpr uint32_t HeaderWaitList = 8;

// Waiters also re-check this often, in case guest code changes a signal state directly.
constexpr auto RecheckInterval = std::chrono::milliseconds(10);

static std::mutex g_lock;
static std::condition_variable g_changed;

static int32_t State(uint32_t object)
{
    return int32_t(GuestLoadU32(object + HeaderSignalState));
}

static void SetState(uint32_t object, int32_t state)
{
    GuestStoreU32(object + HeaderSignalState, uint32_t(state));
}

static bool Signaled(uint32_t object)
{
    return State(object) > 0;
}

// Effect of a satisfied wait on the object.
static void Acquire(uint32_t object)
{
    switch (g_base[object + HeaderType])
    {
    case DispatcherSynchronizationEvent:
        SetState(object, 0);
        break;
    case DispatcherSemaphore:
        SetState(object, State(object) - 1);
        break;
    default: // notification events and threads stay signaled
        break;
    }
}

uint64_t SystemTime100ns()
{
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

Deadline DeadlineFromTimeout(uint32_t timeoutPtr)
{
    if (timeoutPtr == 0)
        return std::nullopt;

    const int64_t value = int64_t((uint64_t(GuestLoadU32(timeoutPtr)) << 32) | GuestLoadU32(timeoutPtr + 4));
    const auto now = std::chrono::steady_clock::now();
    const int64_t relative100ns = value < 0 ? -value : std::max<int64_t>(0, value - int64_t(SystemTime100ns()));
    return now + std::chrono::nanoseconds(relative100ns * 100);
}

void DispatcherInit(uint32_t object, uint8_t type, int32_t signalState)
{
    std::lock_guard lock(g_lock);
    g_base[object + HeaderType] = type;
    g_base[object + 1] = 0;
    g_base[object + HeaderSize] = DispatcherHeaderSize / 4;
    g_base[object + 3] = 0;
    SetState(object, signalState);
    // An empty wait list points at itself.
    GuestStoreU32(object + HeaderWaitList, object + HeaderWaitList);
    GuestStoreU32(object + HeaderWaitList + 4, object + HeaderWaitList);
}

int32_t DispatcherSetState(uint32_t object, int32_t state)
{
    int32_t previous;
    {
        std::lock_guard lock(g_lock);
        previous = State(object);
        SetState(object, state);
    }
    g_changed.notify_all();
    return previous;
}

int32_t DispatcherAddState(uint32_t object, int32_t delta)
{
    int32_t previous;
    {
        std::lock_guard lock(g_lock);
        previous = State(object);
        SetState(object, previous + delta);
    }
    g_changed.notify_all();
    return previous;
}

uint32_t DispatcherWait(const uint32_t* objects, uint32_t count, bool waitAll, Deadline deadline)
{
    std::unique_lock lock(g_lock);
    for (;;)
    {
        if (waitAll)
        {
            bool all = true;
            for (uint32_t i = 0; i < count && all; i++)
                all = Signaled(objects[i]);
            if (all)
            {
                for (uint32_t i = 0; i < count; i++)
                    Acquire(objects[i]);
                return 0;
            }
        }
        else
        {
            for (uint32_t i = 0; i < count; i++)
            {
                if (Signaled(objects[i]))
                {
                    Acquire(objects[i]);
                    return i;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (deadline && now >= *deadline)
            return STATUS_TIMEOUT;

        const auto wake = now + RecheckInterval;
        g_changed.wait_until(lock, deadline && *deadline < wake ? *deadline : wake);
    }
}
