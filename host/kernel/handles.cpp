#include "handles.h"
#include "guest_thread.h"

#include <mutex>
#include <unordered_map>

// Handle values as Xenia hands them out: 0xF8000000 upwards in steps of 4.
constexpr uint32_t FirstHandle = 0xF8000004;

static std::mutex g_mutex;
static std::unordered_map<uint32_t, uint32_t> g_handles;
static uint32_t g_nextHandle = FirstHandle;

uint32_t HandleCreate(uint32_t object)
{
    std::lock_guard lock(g_mutex);
    const uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    g_handles.emplace(handle, object);
    return handle;
}

uint32_t HandleObject(uint32_t handle)
{
    if (handle == CurrentThreadHandle)
    {
        const GuestThread* thread = CurrentGuestThread();
        return thread != nullptr ? thread->threadObject : 0;
    }

    std::lock_guard lock(g_mutex);
    auto it = g_handles.find(handle);
    return it != g_handles.end() ? it->second : 0;
}

bool HandleClose(uint32_t handle)
{
    std::lock_guard lock(g_mutex);
    return g_handles.erase(handle) != 0;
}
