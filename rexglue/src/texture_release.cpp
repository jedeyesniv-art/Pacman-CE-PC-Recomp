// Deferred resource destruction: removes the occasional one-frame stall ("bumps" on the frame graph).
//
// When the game Releases the last reference to a D3D resource (sub_8207DB78), D3D destroys it at once
// (sub_8207D5D8) and first waits for the GPU to stop using it. The game regularly frees textures that were drawn
// in the frame just handed to the GPU (e.g. object destructor sub_82126868 releases three), and the GPU is parked
// on that frame's flip until the next vblank, so the main thread waits a whole extra vblank: the frame is late,
// then the game catches up with two swaps back to back. Traced 2026-10-21 with --pacman_frame_trace.
// The hook (pacman_config.toml, at the destroy call in Release) queues the resource instead - its reference count
// is already 0, so only the GPU can still be using it - and destroys it later, once the GPU is surely done, from
// the same spot the next time the game releases a resource, when it no longer has to wait.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/thread_state.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include <windows.h>  // GetCurrentThreadId

REXCVAR_DEFINE_BOOL(pacman_defer_resource_destroy, true, "Pac-Man",
                    "Destroy released D3D resources a few frames late instead of stalling a frame for the GPU");

// D3D's resource destruction (the game's own code; waits for the GPU, then frees the resource).
extern "C" void sub_8207D5D8(PPCContext& ctx, uint8_t* base);

namespace {

using Clock = std::chrono::steady_clock;

// Long enough for the GPU to finish any frame that used the resource (the CPU runs at most a frame ahead).
constexpr auto DestroyDelay = std::chrono::milliseconds(100);
// Beyond this many waiting resources, destroy normally (a safety net).
constexpr size_t MaxPending = 256;

struct Pending {
  uint32_t resource;
  Clock::time_point queued;
  DWORD thread;  // destroyed on the thread that released it, as D3D would have
};
std::mutex g_mutex;
std::vector<Pending> g_pending;

// Destroys the queued resources that are due and were released on this thread, as D3D itself would.
void DestroyDue(uint8_t* base, Clock::time_point now) {
  std::vector<uint32_t> due;
  const DWORD thread = GetCurrentThreadId();
  {
    std::lock_guard lock(g_mutex);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
      if (it->thread == thread && now - it->queued >= DestroyDelay) {
        due.push_back(it->resource);
        it = g_pending.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (due.empty())
    return;
  PPCContext& ctx = *rex::runtime::current_ppc_context();
  const PPCContext saved = ctx;  // the hook runs mid-function: leave every register as it was
  for (uint32_t resource : due) {
    ctx.r3.u64 = resource;
    sub_8207D5D8(ctx, base);  // may release child resources, re-entering the hook: fine
  }
  ctx = saved;
}

}  // namespace

// At 0x8207DBD8 in Release, where the count has reached 0 and r31 is about to be destroyed. Returns true to skip
// the destroy (jumping past it; Release's result r3 = 0 is set here).
bool DeferDestroyHook(PPCRegister& r3, PPCRegister& r31) {
  if (!REXCVAR_GET(pacman_defer_resource_destroy))
    return false;
  rex::Runtime* runtime = rex::Runtime::instance();
  if (runtime == nullptr)
    return false;
  const uint32_t resource = r31.u32;  // read before DestroyDue, which restores the registers anyway
  const auto now = Clock::now();
  DestroyDue(runtime->virtual_membase(), now);
  {
    std::lock_guard lock(g_mutex);
    if (g_pending.size() >= MaxPending)
      return false;
    g_pending.push_back({resource, now, GetCurrentThreadId()});
  }
  r3.u64 = 0;
  return true;
}
