// Spin-wait backoff for guest polling loops (hooked in pacman_config.toml).
//
// On the Xbox 360 the game's job workers and D3D's GPU wait each had a hardware thread to themselves, so
// they poll with no delay. On a PC each of them pins a core. These hooks keep the first moments of a wait
// as a tight spin (with a CPU pause), so work handed out right away is still picked up within
// microseconds, then sleep in short steps until the wait ends. Game logic is unchanged.

#include <rex/cvar.h>
#include <rex/ppc.h>

#include <chrono>
#include <cstdint>

#include <windows.h> // the SDK build already defines WIN32_LEAN_AND_MEAN and NOMINMAX
#include <immintrin.h>

// Switches for A/B testing (e.g. pacman.exe --pacman_gpu_wait_backoff=false). Off = the original busy-wait.
REXCVAR_DEFINE_BOOL(pacman_worker_backoff, true, "Pac-Man",
                    "Sleep in the job workers' idle polling loops instead of spinning");
REXCVAR_DEFINE_BOOL(pacman_gpu_wait_backoff, true, "Pac-Man",
                    "Sleep in D3D's wait-for-GPU polling loop instead of spinning");

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

// D3D's GPU wait: called once per poll while the CPU waits for the GPU to catch up.
void GpuWaitPollHook() {
  if (!REXCVAR_GET(pacman_gpu_wait_backoff))
    return;
  thread_local uint32_t idlePolls = 0;
  thread_local auto lastPoll = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - lastPoll > NewWaitGap)
    idlePolls = 0;
  IdlePoll(idlePolls, GpuWaitSpinPolls);
  lastPoll = std::chrono::steady_clock::now();
}
