// Sampling profiler for diagnosing slow platforms (Steam Deck / Proton), off by default (--pacman_profile).
// A background thread briefly suspends the main guest thread, the GPU command processor and the UI thread
// every ~2 ms, walks each one's stack and counts where it was. Every 10 s it logs, per thread, the share of
// samples by leaf module (e.g. ntdll = waiting in a system call) and the hottest spots as module+offset of the
// first two frames in our own binaries (pacman.exe, rexruntime.dll, rexgpu-xenos.dll), to be symbolized
// offline with llvm-symbolizer and the matching PDBs.
//
// Nothing that can take a lock runs while a thread is suspended (it could hold that lock): unwind tables are
// looked up in arrays copied from each module's exception directory beforehand, then RtlVirtualUnwind only
// interprets them, and stack reads go through ReadProcessMemory.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

REXCVAR_DEFINE_BOOL(pacman_profile, false, "Pac-Man",
                    "Sample where the main, GPU and UI threads spend their time and log it every 10 s (diagnostics)");

namespace {

constexpr int MaxFrames = 32;
constexpr auto ReportInterval = std::chrono::seconds(10);
constexpr int64_t SampleMicroseconds = 2000;
constexpr size_t TopSpots = 15;

struct Module {
  uintptr_t base = 0;
  uintptr_t end = 0;
  std::string name;
  bool own = false;
  const RUNTIME_FUNCTION* functions = nullptr;  // sorted by BeginAddress (the PE format requires it)
  size_t functionCount = 0;
};

std::vector<Module> g_modules;  // sorted by base; only replaced while no thread is suspended
std::atomic<DWORD> g_uiThreadId{0};
std::atomic<uint32_t> g_unwindFaults{0};  // stack walks cut short by a bad read (see UnwindGuarded)

void LoadModules() {
  std::vector<Module> modules;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE)
    return;
  MODULEENTRY32W entry{sizeof(entry)};
  for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry)) {
    Module module;
    module.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    module.end = module.base + entry.modBaseSize;
    char name[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
    module.name = name;
    std::string lower = module.name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(tolower(c)); });
    module.own = lower == "pacman.exe" || lower == "rexruntime.dll" || lower == "rexgpu-xenos.dll";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (pdata.VirtualAddress != 0 && pdata.Size >= sizeof(RUNTIME_FUNCTION)) {
      module.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + pdata.VirtualAddress);
      module.functionCount = pdata.Size / sizeof(RUNTIME_FUNCTION);
    }
    modules.push_back(std::move(module));
  }
  CloseHandle(snapshot);
  std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
  g_modules = std::move(modules);
}

const Module* FindModule(uintptr_t address) {
  auto it = std::upper_bound(g_modules.begin(), g_modules.end(), address,
                             [](uintptr_t a, const Module& m) { return a < m.base; });
  if (it == g_modules.begin())
    return nullptr;
  --it;
  return address < it->end ? &*it : nullptr;
}

PRUNTIME_FUNCTION FindFunction(const Module& module, uintptr_t address) {
  const DWORD rva = DWORD(address - module.base);
  const RUNTIME_FUNCTION* first = module.functions;
  const RUNTIME_FUNCTION* last = first + module.functionCount;
  auto it = std::upper_bound(first, last, rva, [](DWORD r, const RUNTIME_FUNCTION& f) { return r < f.BeginAddress; });
  if (it == first)
    return nullptr;
  --it;
  return rva < it->EndAddress ? const_cast<PRUNTIME_FUNCTION>(it) : nullptr;
}

// Walks the stack of a suspended thread, storing return addresses (leaf first) and counting them in count.
void Unwind(CONTEXT& context, uintptr_t (&frames)[MaxFrames], int& count) {
  frames[count++] = context.Rip;
  while (count < MaxFrames) {
    const Module* module = FindModule(context.Rip);
    PRUNTIME_FUNCTION function = module != nullptr && module->functions != nullptr
                                     ? FindFunction(*module, context.Rip)
                                     : nullptr;
    if (function != nullptr) {
      void* handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->base, context.Rip, function, &context, &handlerData,
                       &establisher, nullptr);
    } else {
      // A leaf function (no unwind data): the return address is on top of the stack.
      DWORD64 returnAddress = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), &returnAddress,
                             sizeof(returnAddress), nullptr))
        break;
      context.Rip = returnAddress;
      context.Rsp += 8;
    }
    if (context.Rip == 0)
      break;
    frames[count++] = context.Rip;
  }
}

// Unwind, but a bad memory read ends the walk instead of the process. Under Wine a thread suspended in the
// middle of exception dispatch (e.g. a write-watch fault) can have a context whose stack RtlVirtualUnwind can't
// follow; on the Steam Deck that crashed the game within minutes (2026-10-21). No C++ objects here (SEH).
int UnwindGuarded(CONTEXT& context, uintptr_t (&frames)[MaxFrames]) {
  int count = 0;
  __try {
    Unwind(context, frames, count);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                : EXCEPTION_CONTINUE_SEARCH) {
    g_unwindFaults.fetch_add(1, std::memory_order_relaxed);
  }
  return count;
}

struct Sample {
  uintptr_t leaf = 0;
  uintptr_t own[2] = {};  // first two frames in our own modules, leaf side first
};

// Recent main-thread samples with time and deeper stacks, for the frame trace (ProfilerDumpHistory).
struct HistorySample {
  int64_t nanoseconds = 0;
  uint8_t thread = 0;  // 0 = main thread, 1.. = job worker (registration order)
  uintptr_t leaf = 0;
  uintptr_t own[6] = {};
};

// The game's job workers (registered by spin_hooks.cpp's WorkerPollHook on each worker thread).
constexpr size_t MaxWorkers = 16;  // loading spawns short-lived threads that run the worker loop too
std::atomic<DWORD> g_workerIds[MaxWorkers] = {};
std::atomic<uint32_t> g_workerCount{0};

int WorkerIndex(DWORD thread_id) {
  for (uint32_t i = 0; i < g_workerCount.load() && i < MaxWorkers; i++)
    if (g_workerIds[i].load() == thread_id)
      return int(i) + 1;
  return 0;
}
constexpr size_t HistorySize = 8192;
std::mutex g_historyMutex;
HistorySample g_history[HistorySize];
uint64_t g_historyNext = 0;

int64_t SteadyNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// module+offset, without the sampler's module table (safe from any thread).
std::string DescribeAnywhere(uintptr_t address) {
  if (address == 0)
    return "-";
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module))
    return "?";
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  const wchar_t* name = path;
  for (DWORD i = 0; i < length; i++)
    if (path[i] == L'\\' || path[i] == L'/')
      name = path + i + 1;
  char narrow[MAX_PATH];
  WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
  char text[160];
  std::snprintf(text, sizeof(text), "%s+0x%llX", narrow, (unsigned long long)(address - uintptr_t(module)));
  return text;
}

struct Target {
  DWORD id = 0;
  HANDLE handle = nullptr;
  std::string name;
  std::vector<Sample> samples;
};

std::string ThreadName(HANDLE thread) {
  std::string name;
  PWSTR description = nullptr;
  if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
    char narrow[128];
    const int length = WideCharToMultiByte(CP_UTF8, 0, description, -1, narrow, sizeof(narrow), nullptr, nullptr);
    if (length > 1)
      name.assign(narrow, length - 1);
    LocalFree(description);
  }
  return name;
}

// Finds the threads to sample: the main guest thread, the GPU command processor and the UI thread.
void FindTargets(std::vector<Target>& targets) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry{sizeof(entry)};
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == GetCurrentThreadId())
      continue;
    if (std::any_of(targets.begin(), targets.end(), [&](const Target& t) { return t.id == entry.th32ThreadID; }))
      continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, entry.th32ThreadID);
    if (thread == nullptr)
      continue;
    std::string name = ThreadName(thread);
    const int worker = WorkerIndex(entry.th32ThreadID);
    if (worker != 0)
      name = "Job worker " + std::to_string(worker) + " (" + name + ")";
    const bool wanted = name.rfind("Main XThread", 0) == 0 || name.rfind("GPU Commands", 0) == 0 ||
                        entry.th32ThreadID == g_uiThreadId.load() || worker != 0;
    if (!wanted) {
      CloseHandle(thread);
      continue;
    }
    if (name.empty())
      name = "UI thread";
    targets.push_back({entry.th32ThreadID, thread, std::move(name), {}});
  }
  CloseHandle(snapshot);
}

std::string Describe(uintptr_t address) {
  const Module* module = FindModule(address);
  if (module == nullptr)
    return address == 0 ? "-" : "?";
  char text[96];
  std::snprintf(text, sizeof(text), "%s+0x%llX", module->name.c_str(),
                (unsigned long long)(address - module->base));
  return text;
}

void Report(std::vector<Target>& targets) {
  if (const uint32_t faults = g_unwindFaults.exchange(0))
    REXLOG_INFO("profiler: {} stack walks cut short by a bad read", faults);
  for (Target& target : targets) {
    const size_t total = target.samples.size();
    if (total == 0)
      continue;
    std::unordered_map<std::string, size_t> byModule;
    std::unordered_map<std::string, size_t> bySpot;
    for (const Sample& sample : target.samples) {
      const Module* leafModule = FindModule(sample.leaf);
      const std::string leafName = leafModule != nullptr ? leafModule->name : "?";
      byModule[leafName]++;
      bySpot[leafName + " | " + Describe(sample.own[0]) + " <- " + Describe(sample.own[1])]++;
    }
    auto top = [](const std::unordered_map<std::string, size_t>& counts, size_t n) {
      std::vector<std::pair<size_t, std::string>> rows;
      for (const auto& [key, count] : counts)
        rows.emplace_back(count, key);
      std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      rows.resize(std::min(rows.size(), n));
      return rows;
    };
    std::string modules;
    for (const auto& [count, name] : top(byModule, 8))
      modules += (modules.empty() ? "" : ", ") + name + " " + std::to_string(count * 100 / total) + "%";
    REXLOG_INFO("profile [{}] {} samples; leaf modules: {}", target.name, total, modules);
    for (const auto& [count, spot] : top(bySpot, TopSpots))
      REXLOG_INFO("profile [{}] {:5.1f}% {}", target.name, count * 100.0 / total, spot);
    target.samples.clear();
  }
}

void SamplerThread() {
  SetThreadDescription(GetCurrentThread(), L"Pac-Man profiler");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  std::vector<Target> targets;
  LoadModules();
  REXLOG_INFO("profiler: {} modules loaded", g_modules.size());
  FindTargets(targets);
  for (Target& target : targets)
    target.samples.reserve(8192);
  auto lastReport = std::chrono::steady_clock::now();
  REXLOG_INFO("profiler: sampling {} threads every {} us", targets.size(), SampleMicroseconds);

  for (;;) {
    if (timer != nullptr) {
      LARGE_INTEGER due;
      due.QuadPart = -SampleMicroseconds * 10;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    } else {
      Sleep(2);
    }

    for (Target& target : targets) {
      uintptr_t frames[MaxFrames];
      int count = 0;
      if (SuspendThread(target.handle) == DWORD(-1))
        continue;
      CONTEXT context{};
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(target.handle, &context))
        count = UnwindGuarded(context, frames);
      ResumeThread(target.handle);
      if (count == 0)
        continue;

      Sample sample;
      sample.leaf = frames[0];
      int found = 0;
      for (int i = 0; i < count && found < 2; i++) {
        const Module* module = FindModule(frames[i]);
        if (module != nullptr && module->own)
          sample.own[found++] = frames[i];
      }
      target.samples.push_back(sample);

      const bool isMain = target.name.rfind("Main XThread", 0) == 0;
      const int worker = isMain ? 0 : WorkerIndex(target.id);
      if (isMain || worker != 0) {
        HistorySample history;
        history.nanoseconds = SteadyNanoseconds();
        history.thread = uint8_t(worker);
        history.leaf = frames[0];
        int deep = 0;
        for (int i = 0; i < count && deep < 6; i++) {
          const Module* module = FindModule(frames[i]);
          if (module != nullptr && module->own)
            history.own[deep++] = frames[i];
        }
        std::lock_guard lock(g_historyMutex);
        g_history[g_historyNext++ % HistorySize] = history;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= ReportInterval) {
      lastReport = now;
      Report(targets);
      LoadModules();       // pick up late-loaded modules (no thread is suspended here)
      FindTargets(targets);
    }
  }
}

}  // namespace

// Logs the main thread's samples between from and to (steady-clock ns), times relative to center, merging runs of
// identical stacks. Used by the frame trace (frame_stats.cpp) around slow frames.
void ProfilerDumpHistory(int64_t center, int64_t from, int64_t to) {
  std::vector<HistorySample> samples;
  {
    std::lock_guard lock(g_historyMutex);
    for (uint64_t i = g_historyNext > HistorySize ? g_historyNext - HistorySize : 0; i < g_historyNext; i++) {
      const HistorySample& sample = g_history[i % HistorySize];
      if (sample.nanoseconds >= from && sample.nanoseconds <= to)
        samples.push_back(sample);
    }
  }
  for (uint8_t thread = 0; thread <= MaxWorkers; thread++) {
    std::vector<HistorySample> mine;
    for (const HistorySample& sample : samples)
      if (sample.thread == thread)
        mine.push_back(sample);
    const std::string label = thread == 0 ? "main" : "worker " + std::to_string(thread);
    for (size_t i = 0; i < mine.size();) {
      size_t j = i + 1;
      while (j < mine.size() && mine[j].leaf == mine[i].leaf &&
             std::equal(std::begin(mine[j].own), std::end(mine[j].own), std::begin(mine[i].own)))
        j++;
      std::string stack = DescribeAnywhere(mine[i].leaf);
      for (uintptr_t frame : mine[i].own)
        if (frame != 0)
          stack += " <- " + DescribeAnywhere(frame);
      REXLOG_INFO("profile history [{}] {:+.1f}..{:+.1f} ms ({} samples): {}", label,
                  (mine[i].nanoseconds - center) / 1e6, (mine[j - 1].nanoseconds - center) / 1e6, j - i, stack);
      i = j;
    }
  }
}

// Called once on each job worker thread (spin_hooks.cpp), so the profiler samples the workers too.
void ProfilerNoteWorkerThread() {
  const DWORD id = GetCurrentThreadId();
  if (WorkerIndex(id) != 0)
    return;
  const uint32_t slot = g_workerCount.fetch_add(1);
  if (slot < MaxWorkers)
    g_workerIds[slot] = id;
}

// Called on the UI thread at startup, so the UI thread can be sampled too.
void ProfilerNoteUiThread() { g_uiThreadId = GetCurrentThreadId(); }

// Called from FrameHook on each frame; starts the sampler once, after the game is up.
void ProfilerMaybeStart() {
  static bool started = false;
  if (started || !REXCVAR_GET(pacman_profile))
    return;
  started = true;
  REXLOG_INFO("profiler: starting");
  std::thread(SamplerThread).detach();
}
