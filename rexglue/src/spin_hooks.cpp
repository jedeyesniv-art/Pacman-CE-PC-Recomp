// Spin-wait backoff for guest polling loops (hooked in pacman_config.toml).
//
// On the Xbox 360 the game's job workers and D3D's GPU wait each had a hardware thread to themselves, so
// they poll with no delay. On a PC each of them pins a core. These hooks keep the first moments of a wait
// as a tight spin (with a CPU pause), so work handed out right away is still picked up within
// microseconds, then sleep in short steps until the wait ends. Game logic is unchanged.

#include <rex/cvar.h>
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
// On by default: it doesn't remove the gameplay slowdown but makes it much milder (owner-tested 2026-10-20:
// with it off, lag was constant). Costs ~1-2% CPU (the command processor polls instead of sleeping).
REXCVAR_DEFINE_BOOL(pacman_fast_gpu_handshake, true, "Pac-Man",
                    "Make the GPU poll (not sleep 1 ms) in D3D's per-frame CPU/GPU handshake waits");

namespace {

// Idle polls spent spinning before sleeping (a pause is roughly 10-50 ns, so this is tens of microseconds).
constexpr uint32_t WorkerSpinPolls = 2000;
// GPU wait polls already include the guest's own delay loop, so fewer of them before sleeping.
constexpr uint32_t GpuWaitSpinPolls = 200;
// Length of each sleep once spinning is over.
constexpr int64_t SleepMicroseconds = 250;
// A GPU wait poll after this long a gap starts a new wait (D3D waits about once per frame).
constexpr auto NewWaitGap = std::chrono::milliseconds(2);

// Sleeps for about SleepMicroseconds. Sleep(1) would round up to a whole millisecond or more, so this uses
// a high-resolution waitable timer (one per thread), falling back to Sleep(1) if it isn't available.
void ShortSleep() {
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                     TIMER_ALL_ACCESS);
  if (timer == nullptr) {
    Sleep(1);
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -SleepMicroseconds * 10;  // relative, 100 ns units
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
void GpuWaitPollHook() {
  thread_local uint32_t idlePolls = 0;
  thread_local auto lastPoll = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  const auto sinceLast = now - lastPoll;
  if (sinceLast > NewWaitGap)
    idlePolls = 0;  // a new wait
  else
    g_gpuWaitNanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(sinceLast).count(),
                                   std::memory_order_relaxed);
  lastPoll = now;  // gaps run start to start, so they include any sleep below
  if (REXCVAR_GET(pacman_gpu_wait_backoff))
    IdlePoll(idlePolls, GpuWaitSpinPolls);
}
