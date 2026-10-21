// Spin-wait backoff for guest polling loops (hooked in pacman_config.toml).
//
// On the Xbox 360 the game's job workers and D3D's GPU wait each had a hardware thread to themselves, so
// they poll with no delay. On a PC each of them pins a core. These hooks keep the first moments of a wait
// as a tight spin (with a CPU pause), so work handed out right away is still picked up within
// microseconds, then sleep in short steps until the wait ends. Game logic is unchanged.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include <windows.h> // the SDK build already defines WIN32_LEAN_AND_MEAN and NOMINMAX
#include <immintrin.h>

// Switches for A/B testing (e.g. pacman.exe --pacman_gpu_wait_backoff=false). Off = the original busy-wait.
REXCVAR_DEFINE_BOOL(pacman_worker_backoff, true, "Pac-Man",
                    "Sleep in the job workers' idle polling loops instead of spinning");
REXCVAR_DEFINE_BOOL(pacman_gpu_wait_backoff, true, "Pac-Man",
                    "Sleep in D3D's wait-for-GPU polling loop instead of spinning");
// Off by default since presentation was moved to the guest thread (the real cause of the gameplay slowdown,
// see pacman_app.h): re-measured 2026-10-20, it no longer changes frame pacing but keeps the GPU thread
// busy-polling at ~95% of a core. It was a mitigation while presentation waited on the host refresh.
REXCVAR_DEFINE_BOOL(pacman_fast_gpu_handshake, false, "Pac-Man",
                    "Make the GPU poll (not sleep 1 ms) in D3D's per-frame CPU/GPU handshake waits");
REXCVAR_DEFINE_UINT32(pacman_command_buffer_mb, 8, "Pac-Man",
                      "D3D command buffer size in MB (the game's default is 2; 2 restores it)");
REXCVAR_DEFINE_UINT32(pacman_backoff_sleep_us, 250, "Pac-Man",
                      "Length of each sleep step in the spin-wait backoff, in microseconds");

namespace {

// Idle polls spent spinning before sleeping (a pause is roughly 10-50 ns, so this is tens of microseconds).
constexpr uint32_t WorkerSpinPolls = 2000;
// GPU wait polls already include the guest's own delay loop, so fewer of them before sleeping.
constexpr uint32_t GpuWaitSpinPolls = 200;
// A GPU wait poll after this long a gap starts a new wait (D3D waits about once per frame).
constexpr auto NewWaitGap = std::chrono::milliseconds(2);

// Sleeps for about pacman_backoff_sleep_us. Sleep(1) would round up to a whole millisecond or more, so this uses
// a high-resolution waitable timer (one per thread), falling back to Sleep(1) if it isn't available.
void ShortSleep() {
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                     TIMER_ALL_ACCESS);
  if (timer == nullptr) {
    Sleep(1);
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -int64_t(REXCVAR_GET(pacman_backoff_sleep_us)) * 10;  // relative, 100 ns units
  if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
    WaitForSingleObject(timer, INFINITE);
}

void IdlePoll(uint32_t& idlePolls, uint32_t spinPolls) {
  if (idlePolls < spinPolls) {
    idlePolls++;
    _mm_pause();
  } else {
    ShortSleep();
  }
}

}  // namespace

// Job workers: r11 is the worker's control word; 1 means a job has been handed out.
void WorkerPollHook(PPCRegister& r11) {
  if (!REXCVAR_GET(pacman_worker_backoff))
    return;
  thread_local uint32_t idlePolls = 0;
  if (r11.u32 == 1)
    idlePolls = 0;
  else
    IdlePoll(idlePolls, WorkerSpinPolls);
}

// D3D's command buffer: the game uses the defaults (2 MB in 32 segments of 64 KB). Heavy frames overflow the
// free space while the GPU waits for the previous frame's flip, stalling the CPU mid-frame. Enlarge it, keeping
// the 64 KB segment size. Only applied when the game left both at their defaults.
void CommandBufferSizeHook(PPCRegister& size, PPCRegister& segments) {
  constexpr uint32_t DefaultSize = 0x200000;
  constexpr uint32_t SegmentSize = 0x10000;
  const uint32_t megabytes = REXCVAR_GET(pacman_command_buffer_mb);
  if (size.u32 != DefaultSize || segments.u32 != 0 || megabytes <= 2 || megabytes > 64)
    return;
  size.u32 = megabytes * 0x100000;
  segments.u32 = size.u32 / SegmentSize;
  REXLOG_INFO("D3D command buffer: {} MB in {} segments (game default 2 MB)", megabytes, segments.u32);
}

// The wait field of D3D's per-frame handshake WAIT_REG_MEM packets. The command processor sleeps
// wait/0x100 ms when the condition isn't met yet, but only yields for wait < 0x100, so it notices the CPU's
// acknowledgement within microseconds instead of a millisecond or more later.
void GpuHandshakeWaitHook(PPCRegister& wait) {
  if (REXCVAR_GET(pacman_fast_gpu_handshake) && wait.u32 >= 0x100)
    wait.u32 = 0xFF;
}

// Total time the CPU has spent in D3D's GPU wait, for frame_stats.cpp: the gaps between consecutive polls of
// the same wait. Measured whether or not the backoff is on, so A/B runs are comparable.
static std::atomic<uint64_t> g_gpuWaitNanoseconds{0};

uint64_t GpuWaitNanoseconds() {
  return g_gpuWaitNanoseconds.load(std::memory_order_relaxed);
}

// D3D's GPU wait: called once per poll while the CPU waits for the GPU to catch up.
// Frame trace: each poll of a GPU wait (frame_stats.cpp).
void FrameTraceGpuWaitPoll(uint32_t r1, bool newWait);

void GpuWaitPollHook(PPCRegister& r1) {
  thread_local uint32_t idlePolls = 0;
  thread_local auto lastPoll = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  const auto sinceLast = now - lastPoll;
  const bool newWait = sinceLast > NewWaitGap;
  if (newWait)
    idlePolls = 0;
  FrameTraceGpuWaitPoll(r1.u32, newWait);
  if (!newWait)
    g_gpuWaitNanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(sinceLast).count(),
                                   std::memory_order_relaxed);
  lastPoll = now;  // gaps run start to start, so they include any sleep below
  if (REXCVAR_GET(pacman_gpu_wait_backoff))
    IdlePoll(idlePolls, GpuWaitSpinPolls);
}
