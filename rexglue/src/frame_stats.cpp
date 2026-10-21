// Frame-time monitor: the game advances one step per frame it presents (VdSwap), timed by the 60 Hz guest
// vblank, so any frame that takes longer than its slot slows the game down while music keeps real time.
// FrameHook (hooked on the game's only VdSwap call, see pacman_config.toml) logs a summary every 10 s:
// frames, frames that missed their slot, the worst frame, and how far game time has fallen behind.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
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
// Starts the sampling profiler if --pacman_profile is set (profiler.cpp).
void ProfilerMaybeStart();
// Main-thread samples in a time window, logged (profiler.cpp).
void ProfilerDumpHistory(int64_t center, int64_t from, int64_t to);
// Installs the crash logger (diagnostics.cpp).
void DiagnosticsInstall();

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

// Frame trace (--pacman_frame_trace): timestamps of the events that pace a frame, kept in a ring buffer and
// dumped around slow frames (VdSwap interval over 20 ms) to see which wait made the frame late.
REXCVAR_DEFINE_BOOL(pacman_frame_trace, false, "Pac-Man",
                    "Log a timeline of vblanks, swaps and GPU waits around slow frames (diagnostics)");
namespace {
enum TraceKind : uint8_t { TraceVblank, TraceSwap, TraceCommandIrq, TraceFenceBegin, TraceFenceEnd,
                           TraceBufferBegin, TraceBufferEnd, TraceFenceCall, TraceGpuWait, TraceJobsStart,
                           TraceJobsDone };
constexpr char TraceLetters[] = {'V', 'S', 'C', '(', ')', '[', ']', 'F', '<', 'D', 'd'};
struct TraceEvent {
  int64_t nanoseconds;
  TraceKind kind;
  uint8_t site;          // TraceFenceCall: which call of the fence wait (FenceCallSites)
  uint32_t callers[3];  // TraceGpuWait: guest return addresses, innermost first
};
constexpr size_t TraceSize = 4096;
TraceEvent g_trace[TraceSize];
std::atomic<uint64_t> g_traceNext{0};

int64_t NowNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Trace(TraceKind kind, uint8_t site = 0, const uint32_t* callers = nullptr) {
  if (!REXCVAR_GET(pacman_frame_trace))
    return;
  const uint64_t index = g_traceNext.fetch_add(1, std::memory_order_relaxed);
  TraceEvent event{NowNanoseconds(), kind, site, {}};
  if (callers != nullptr)
    std::copy(callers, callers + 3, event.callers);
  g_trace[index % TraceSize] = event;
}

// Guest return addresses from the stack's back chain (each frame's saved LR is at back chain - 8).
void GuestCallers(uint32_t sp, uint32_t (&callers)[3]) {
  rex::Runtime* runtime = rex::Runtime::instance();
  if (runtime == nullptr)
    return;
  const uint8_t* base = runtime->virtual_membase();
  auto read32 = [base](uint32_t address) { return _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + address)); };
  for (int depth = 0; depth < 3 && sp != 0 && sp < 0xF0000000; depth++) {
    const uint32_t back = read32(sp);
    if (back <= sp)
      break;
    callers[depth] = read32(back - 8);
    sp = back;
  }
}

// Logs the events from 70 ms before to 40 ms after the slow frame's swap at `center`, as ms relative to it:
// V vblank, S swap (VdSwap), C command-buffer interrupt, ( ) fence wait, [ ] command-buffer-full wait,
// Fn call of the fence-wait function from call site n (FenceCallSites; it returns at once if already passed),
// <[a,b,c] start of a D3D GPU wait (sub_82065018 polls) with its guest callers,
// D d the game's job dispatcher (sub_8213ECA8) starting jobs on the 3 workers / all of them done.
void DumpTrace(int64_t center, double frameMs) {
  std::vector<TraceEvent> events;
  const uint64_t end = g_traceNext.load(std::memory_order_relaxed);
  for (uint64_t i = end > TraceSize ? end - TraceSize : 0; i < end; i++) {
    const TraceEvent& e = g_trace[i % TraceSize];
    if (e.nanoseconds >= center - 70'000'000 && e.nanoseconds <= center + 40'000'000)
      events.push_back(e);
  }
  std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.nanoseconds < b.nanoseconds; });
  std::string text;
  char item[64];
  for (const TraceEvent& e : events) {
    if (e.kind == TraceGpuWait)
      std::snprintf(item, sizeof(item), " <%+.1f[%X,%X,%X]", (e.nanoseconds - center) / 1e6, e.callers[0],
                    e.callers[1], e.callers[2]);
    else if (e.kind == TraceFenceCall)
      std::snprintf(item, sizeof(item), " F%u%+.1f", e.site, (e.nanoseconds - center) / 1e6);
    else
      std::snprintf(item, sizeof(item), " %c%+.1f", TraceLetters[e.kind], (e.nanoseconds - center) / 1e6);
    text += item;
  }
  REXLOG_INFO("frame trace (slow frame {:.1f} ms, swap at 0):{}", frameMs, text);
  ProfilerDumpHistory(center, center - 40'000'000, center + 2'000'000);  // with --pacman_profile
}
}  // namespace

// Each poll of a D3D GPU wait (spin_hooks.cpp GpuWaitPollHook; r1 = sub_82065018's stack pointer): marks the start
// of a wait - after a pause between polls, or when the calling code changes (waits can follow back to back).
void FrameTraceGpuWaitPoll(uint32_t r1, bool newWait) {
  if (!REXCVAR_GET(pacman_frame_trace))
    return;
  thread_local uint32_t lastCallers[3] = {};
  uint32_t callers[3] = {};
  GuestCallers(r1, callers);
  if (newWait || !std::equal(callers, callers + 3, lastCallers))
    Trace(TraceGpuWait, 0, callers);
  std::copy(callers, callers + 3, lastCallers);
}

// The game's job dispatcher sub_8213ECA8: jobs handed out (it then spins until all 3 workers are done) / done.
void JobsStartHook() { Trace(TraceJobsStart); }
void JobsDoneHook() { Trace(TraceJobsDone); }

// Calls of the fence-wait function sub_8206F728 (the bl instructions; see pacman_config.toml), for the trace.
#define PACMAN_FENCE_CALL_HOOK(index, site) \
  void FenceCallHook_##site() { Trace(TraceFenceCall, index); }
PACMAN_FENCE_CALL_HOOK(0, 82067F30)
PACMAN_FENCE_CALL_HOOK(1, 8206FBBC)
PACMAN_FENCE_CALL_HOOK(2, 8206FE14)
PACMAN_FENCE_CALL_HOOK(3, 82070010)
// Site 4 is in D3D's resource destruction (sub_8207D5D8, from Release sub_8207DB78): the game freed a
// resource the GPU may still be using. Also logs who released it, by walking the guest stack's back chain (each
// frame's saved LR is at back chain - 8), and the resource type (common word & 0xF).
void FenceCallHook_8207D690(PPCRegister& r1, PPCRegister& r31) {
  Trace(TraceFenceCall, 4);
  static int logged = 0;
  if (!REXCVAR_GET(pacman_frame_trace) || logged >= 40)
    return;
  logged++;
  rex::Runtime* runtime = rex::Runtime::instance();
  if (runtime == nullptr)
    return;
  const uint8_t* base = runtime->virtual_membase();
  auto read32 = [base](uint32_t address) { return _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + address)); };
  std::string chain;
  uint32_t sp = r1.u32;
  for (int depth = 0; depth < 5 && sp != 0 && sp < 0xF0000000; depth++) {
    const uint32_t back = read32(sp);
    if (back <= sp)
      break;
    char item[16];
    std::snprintf(item, sizeof(item), " %08X", read32(back - 8));
    chain += item;
    sp = back;
  }
  REXLOG_INFO("frame trace: resource {:08X} (type {}) destroyed; return addresses:{}", r31.u32,
              read32(r31.u32) & 0xF, chain);
}
PACMAN_FENCE_CALL_HOOK(5, 8207D6DC)
PACMAN_FENCE_CALL_HOOK(6, 8207D70C)
PACMAN_FENCE_CALL_HOOK(7, 8207D738)
PACMAN_FENCE_CALL_HOOK(8, 8207D79C)
PACMAN_FENCE_CALL_HOOK(9, 8207D84C)

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

void BufferWaitBeginHook() {
  t_bufferWaitStart = std::chrono::steady_clock::now();
  Trace(TraceBufferBegin);
}
void BufferWaitEndHook() {
  EndWait(g_bufferWait, t_bufferWaitStart);
  Trace(TraceBufferEnd);
}
void FenceWaitBeginHook() {
  t_fenceWaitStart = std::chrono::steady_clock::now();
  Trace(TraceFenceBegin);
}
void FenceWaitEndHook() {
  EndWait(g_fenceWait, t_fenceWaitStart);
  Trace(TraceFenceEnd);
}

// Draw calls (sub_820CB840), counted per frame by FrameHook.
namespace {
std::atomic<uint32_t> g_draws{0};
}
void DrawHook() { g_draws.fetch_add(1, std::memory_order_relaxed); }

// Called by FrameHook at each VdSwap.
void NoteSwap(std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(g_vblankMutex);
  g_vblank.lastSwap = now;
}

// Graphics interrupt callback entry: r3 = source, 0 = vblank, 1 = command buffer (PM4_INTERRUPT).
void InterruptHook(PPCRegister& r3) {
  if (r3.u32 <= 1)
    Trace(r3.u32 == 0 ? TraceVblank : TraceCommandIrq);
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

// Access violations, counted by a first-in-line vectored handler that only counts and passes them on. Guest
// writes to GPU registers (and other MMIO) are emulated through deliberate faults; each is cheap on Windows
// but goes through a Linux signal under Wine/Proton, so the count per frame shows what that path costs.
// Kinds, matching ReXGlue's MMIOHandler: GPU registers (guest 0x7FC80000-0x7FC8FFFF, emulated on every
// access), other guest memory (write watches: the GPU emulation protects memory it has cached, e.g. vertex
// data, to notice the game rewriting it), anything else.
enum FaultKind { GpuRegisterFault, WatchFault, OtherFault, FaultKinds };
std::atomic<uint64_t> g_faults[FaultKinds]{};
std::atomic<uint64_t> g_mainThreadFaults{0};
std::atomic<DWORD> g_mainThreadId{0};
std::atomic<uintptr_t> g_virtualMembase{0};

LONG CALLBACK CountAccessViolation(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t address = record.ExceptionInformation[1];
  const uintptr_t virtualBase = g_virtualMembase.load(std::memory_order_relaxed);
  FaultKind kind = OtherFault;
  if (virtualBase != 0 && address >= virtualBase && address - virtualBase < 0x200000000ull) {
    const uint64_t offset = address - virtualBase;  // the physical view starts 4 GB above the virtual one
    kind = offset < 0x100000000ull && (offset & 0xFFFF0000) == 0x7FC80000 ? GpuRegisterFault : WatchFault;
  }
  g_faults[kind].fetch_add(1, std::memory_order_relaxed);
  if (GetCurrentThreadId() == g_mainThreadId.load(std::memory_order_relaxed))
    g_mainThreadFaults.fetch_add(1, std::memory_order_relaxed);
  return EXCEPTION_CONTINUE_SEARCH;
}

std::string TakeFaultSummary(uint32_t frames) {
  static uint64_t last[FaultKinds]{}, lastMain = 0;
  uint64_t now[FaultKinds], total = 0;
  for (int i = 0; i < FaultKinds; i++) {
    now[i] = g_faults[i].load(std::memory_order_relaxed);
    total += now[i] - last[i];
  }
  const uint64_t main = g_mainThreadFaults.load(std::memory_order_relaxed);
  char text[192];
  std::snprintf(text, sizeof(text),
                "access faults %llu (%.0f per frame: GPU registers %llu, write watches %llu, other %llu; "
                "%llu on the main thread)",
                (unsigned long long)total, frames ? double(total) / frames : 0.0,
                (unsigned long long)(now[GpuRegisterFault] - last[GpuRegisterFault]),
                (unsigned long long)(now[WatchFault] - last[WatchFault]),
                (unsigned long long)(now[OtherFault] - last[OtherFault]), (unsigned long long)(main - lastMain));
  for (int i = 0; i < FaultKinds; i++)
    last[i] = now[i];
  lastMain = main;
  return text;
}

using Clock = std::chrono::steady_clock;

constexpr double GuestFrameSeconds = 1.0 / 60.0;
constexpr double SlowFrameSeconds = 0.020;  // a frame this long has missed its 16.7 ms slot
constexpr double FastFrameSeconds = 0.010;  // a frame this short is catching up after a slow one
constexpr auto ReportInterval = std::chrono::seconds(10);

struct Window {
  Clock::time_point start;
  uint32_t frames = 0;
  uint32_t slowFrames = 0;
  uint32_t fastFrames = 0;  // under FastFrameSeconds: catching up after a slow one
  double worstSeconds = 0;
  uint64_t draws = 0;
  uint32_t maxDraws = 0;
};

}  // namespace

void FrameHook() {
  ProfilerMaybeStart();
  DiagnosticsInstall();
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
    if (rex::Runtime* runtime = rex::Runtime::instance())
      g_virtualMembase = reinterpret_cast<uintptr_t>(runtime->virtual_membase());
    g_mainThreadId = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, CountAccessViolation);
    return;
  }

  NoteSwap(now);
  Trace(TraceSwap);
  const double frameSeconds = std::chrono::duration<double>(now - last).count();
  last = now;

  // Frame trace: dump around a slow frame once 40 ms have passed after it (at most 80 per run).
  static int64_t pendingDump = 0;
  static double pendingMs = 0;
  static int dumps = 0;
  const int64_t nowNs = NowNanoseconds();
  if (pendingDump != 0 && nowNs - pendingDump > 40'000'000) {
    DumpTrace(pendingDump, pendingMs);
    pendingDump = 0;
  }
  if (REXCVAR_GET(pacman_frame_trace) && frameSeconds > SlowFrameSeconds && pendingDump == 0 && dumps < 80) {
    pendingDump = nowNs;
    pendingMs = frameSeconds * 1000;
    dumps++;
  }
  window.frames++;
  if (frameSeconds > SlowFrameSeconds)
    window.slowFrames++;
  else if (frameSeconds < FastFrameSeconds)
    window.fastFrames++;
  window.worstSeconds = std::max(window.worstSeconds, frameSeconds);
  const uint32_t draws = g_draws.exchange(0, std::memory_order_relaxed);
  window.draws += draws;
  window.maxDraws = std::max(window.maxDraws, draws);

  if (now - window.start >= ReportInterval) {
    const double elapsed = std::chrono::duration<double>(now - window.start).count();
    // Game time advances one guest frame per presented frame; anything beyond that is time lost.
    const double lost = std::max(0.0, elapsed - window.frames * GuestFrameSeconds);
    totalLostSeconds += lost;
    const uint64_t gpuWait = GpuWaitNanoseconds();
    const double gpuWaitShare = (gpuWait - gpuWaitAtStart) / 1e9 / elapsed;
    gpuWaitAtStart = gpuWait;
    // The report is put together and logged on a short-lived thread: listing the threads and their CPU times
    // takes long enough to make this frame miss its vblank (it caused one bump every 10 s, seen 2026-10-21).
    std::thread([window = window, elapsed, lost, total = totalLostSeconds, gpuWaitShare]() {
      REXLOG_INFO("frames: {} in {:.1f} s ({:.1f} fps), {} over {:.0f} ms, {} under {:.0f} ms, worst {:.1f} ms; game "
                  "behind by {:.0f} ms this window, {:.1f} s total; draws {:.0f} per frame (max {}); {}; {}; "
                  "main thread waiting on GPU {:.0f}%; {}; busiest: {}",
                  window.frames, elapsed, window.frames / elapsed, window.slowFrames, SlowFrameSeconds * 1000,
                  window.fastFrames, FastFrameSeconds * 1000, window.worstSeconds * 1000, lost * 1000, total,
                  window.frames ? double(window.draws) / window.frames : 0.0, window.maxDraws, TakeVblankSummary(),
                  TakeWaitSummary(), gpuWaitShare * 100, TakeFaultSummary(window.frames), BusiestThreads(elapsed));
    }).detach();
    window = Window{now};
  }
}
