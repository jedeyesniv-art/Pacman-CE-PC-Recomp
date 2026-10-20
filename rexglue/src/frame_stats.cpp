// Frame-time monitor: the game advances one step per frame it presents (VdSwap), timed by the 60 Hz guest
// vblank, so any frame that takes longer than its slot slows the game down while music keeps real time.
// FrameHook (hooked on the game's only VdSwap call, see pacman_config.toml) logs a summary every 10 s:
// frames, frames that missed their slot, the worst frame, and how far game time has fallen behind.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

REXCVAR_DEFINE_BOOL(pacman_frame_stats, true, "Pac-Man",
                    "Log a frame-time summary every 10 s (frames, slow frames, game time lost)");

// Total time spent in D3D's GPU wait (spin_hooks.cpp).
uint64_t GpuWaitNanoseconds();

namespace {

// Vblank interrupts as the game receives them (InterruptHook), read and reset by each FrameHook report.
struct VblankStats {
  std::chrono::steady_clock::time_point last;
  uint32_t count = 0;
  uint32_t late = 0;  // gaps over SlowFrameSeconds
  double worstGapSeconds = 0;
  // Command-buffer interrupts (source 1): delay from the preceding VdSwap.
  std::chrono::steady_clock::time_point lastSwap;
  uint32_t commandCount = 0;
  double commandDelaySum = 0;
  double commandDelayWorst = 0;
};
std::mutex g_vblankMutex;
VblankStats g_vblank;

}  // namespace

// Time spent in D3D's two stalling waits (hooked on their slow paths), read and reset by each report.
namespace {
struct WaitTotals {
  std::atomic<uint64_t> nanoseconds{0};
  std::atomic<uint32_t> count{0};
};
WaitTotals g_bufferWait;
WaitTotals g_fenceWait;
thread_local std::chrono::steady_clock::time_point t_bufferWaitStart;
thread_local std::chrono::steady_clock::time_point t_fenceWaitStart;

void EndWait(WaitTotals& totals, std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  totals.nanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(),
                               std::memory_order_relaxed);
  totals.count.fetch_add(1, std::memory_order_relaxed);
}

std::string TakeWaitSummary() {
  char text[160];
  std::snprintf(text, sizeof(text), "buffer-full waits %u (%.0f ms), fence waits %u (%.0f ms)",
                g_bufferWait.count.exchange(0), g_bufferWait.nanoseconds.exchange(0) / 1e6,
                g_fenceWait.count.exchange(0), g_fenceWait.nanoseconds.exchange(0) / 1e6);
  return text;
}
}  // namespace

void BufferWaitBeginHook() { t_bufferWaitStart = std::chrono::steady_clock::now(); }
void BufferWaitEndHook() { EndWait(g_bufferWait, t_bufferWaitStart); }
void FenceWaitBeginHook() { t_fenceWaitStart = std::chrono::steady_clock::now(); }
void FenceWaitEndHook() { EndWait(g_fenceWait, t_fenceWaitStart); }

// Called by FrameHook at each VdSwap.
void NoteSwap(std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(g_vblankMutex);
  g_vblank.lastSwap = now;
}

// Graphics interrupt callback entry: r3 = source, 0 = vblank, 1 = command buffer (PM4_INTERRUPT).
void InterruptHook(PPCRegister& r3) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_vblankMutex);
  if (r3.u32 == 1) {
    if (g_vblank.lastSwap != std::chrono::steady_clock::time_point{}) {
      const double delay = std::chrono::duration<double>(now - g_vblank.lastSwap).count();
      g_vblank.commandCount++;
      g_vblank.commandDelaySum += delay;
      g_vblank.commandDelayWorst = std::max(g_vblank.commandDelayWorst, delay);
    }
    return;
  }
  if (r3.u32 != 0)
    return;
  if (g_vblank.last != std::chrono::steady_clock::time_point{}) {
    const double gap = std::chrono::duration<double>(now - g_vblank.last).count();
    g_vblank.worstGapSeconds = std::max(g_vblank.worstGapSeconds, gap);
    if (gap > 0.020)
      g_vblank.late++;
  }
  g_vblank.last = now;
  g_vblank.count++;
}

namespace {

// Vblank summary since the previous call, and reset the counters (keeping the last timestamp).
std::string TakeVblankSummary() {
  std::lock_guard lock(g_vblankMutex);
  char text[200];
  std::snprintf(text, sizeof(text),
                "%u vblank interrupts, %u late, max gap %.0f ms; %u command-buffer interrupts, %.1f ms avg / "
                "%.1f ms max after VdSwap",
                g_vblank.count, g_vblank.late, g_vblank.worstGapSeconds * 1000, g_vblank.commandCount,
                g_vblank.commandCount ? g_vblank.commandDelaySum / g_vblank.commandCount * 1000 : 0.0,
                g_vblank.commandDelayWorst * 1000);
  g_vblank.count = 0;
  g_vblank.late = 0;
  g_vblank.worstGapSeconds = 0;
  g_vblank.commandCount = 0;
  g_vblank.commandDelaySum = 0;
  g_vblank.commandDelayWorst = 0;
  return text;
}

// The busiest threads of this process since the previous call, as "name NN%" (percent of one core).
std::string BusiestThreads(double seconds) {
  constexpr size_t Shown = 4;
  static std::unordered_map<DWORD, uint64_t> previous;  // thread id -> CPU time, 100 ns units

  std::unordered_map<DWORD, uint64_t> current;
  std::vector<std::pair<double, std::string>> rows;
  const DWORD pid = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry{sizeof(entry)};
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid)
      continue;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
    if (thread == nullptr)
      continue;

    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
      const uint64_t cpu = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                           ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
      current[entry.th32ThreadID] = cpu;
      auto before = previous.find(entry.th32ThreadID);
      if (before != previous.end()) {
        std::string name = "thread " + std::to_string(entry.th32ThreadID);
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
          if (description[0] != L'\0') {
            char narrow[128];
            const int length = WideCharToMultiByte(CP_UTF8, 0, description, -1, narrow, sizeof(narrow), nullptr, nullptr);
            if (length > 1)
              name.assign(narrow, length - 1);
          }
          LocalFree(description);
        }
        rows.emplace_back((cpu - before->second) / 1e7 / seconds * 100, std::move(name));
      }
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  previous = std::move(current);

  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string text;
  for (size_t i = 0; i < rows.size() && i < Shown; i++) {
    if (!text.empty())
      text += ", ";
    text += rows[i].second + " " + std::to_string(int(rows[i].first + 0.5)) + "%";
  }
  return text;
}

using Clock = std::chrono::steady_clock;

constexpr double GuestFrameSeconds = 1.0 / 60.0;
constexpr double SlowFrameSeconds = 0.020;  // a frame this long has missed its 16.7 ms slot
constexpr auto ReportInterval = std::chrono::seconds(10);

struct Window {
  Clock::time_point start;
  uint32_t frames = 0;
  uint32_t slowFrames = 0;
  double worstSeconds = 0;
};

}  // namespace

void FrameHook() {
  if (!REXCVAR_GET(pacman_frame_stats))
    return;

  static Clock::time_point last;
  static Window window;
  static double totalLostSeconds = 0;
  static uint64_t gpuWaitAtStart = 0;

  const auto now = Clock::now();
  if (last == Clock::time_point{}) {
    last = now;
    window.start = now;
    gpuWaitAtStart = GpuWaitNanoseconds();
    BusiestThreads(1.0);  // take the baseline
    return;
  }

  NoteSwap(now);
  const double frameSeconds = std::chrono::duration<double>(now - last).count();
  last = now;
  window.frames++;
  if (frameSeconds > SlowFrameSeconds)
    window.slowFrames++;
  window.worstSeconds = std::max(window.worstSeconds, frameSeconds);

  if (now - window.start >= ReportInterval) {
    const double elapsed = std::chrono::duration<double>(now - window.start).count();
    // Game time advances one guest frame per presented frame; anything beyond that is time lost.
    const double lost = std::max(0.0, elapsed - window.frames * GuestFrameSeconds);
    totalLostSeconds += lost;
    const uint64_t gpuWait = GpuWaitNanoseconds();
    const double gpuWaitShare = (gpuWait - gpuWaitAtStart) / 1e9 / elapsed;
    gpuWaitAtStart = gpuWait;
    REXLOG_INFO("frames: {} in {:.1f} s ({:.1f} fps), {} over {:.0f} ms, worst {:.1f} ms; game behind by "
                "{:.0f} ms this window, {:.1f} s total; {}; {}; main thread waiting on GPU {:.0f}%; busiest: {}",
                window.frames, elapsed, window.frames / elapsed, window.slowFrames, SlowFrameSeconds * 1000,
                window.worstSeconds * 1000, lost * 1000, totalLostSeconds, TakeVblankSummary(),
                TakeWaitSummary(), gpuWaitShare * 100, BusiestThreads(elapsed));
    window = Window{now};
  }
}
