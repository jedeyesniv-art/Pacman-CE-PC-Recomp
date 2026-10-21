// Diagnostics for unexplained exits (first seen on the Steam Deck, 2026-10-21: the game ended after ~3 min of
// normal play with only "Execution complete" in the log, the same line a normal quit gives).
// - StopHook_*: mid-asm hooks on every call the game makes to a function that stops it or its thread
//   (KeBugCheck/Ex, XamLoaderTerminateTitle, ExTerminateThread, RtlRaiseException); the SDK logs these at debug
//   level at most. Each logs its call site and arguments as an error and flushes the log.
// - An unhandled-exception filter logs any host crash (code, address, stack as module+offset, to symbolize
//   with llvm-symbolizer and the matching PDBs) before the process dies.

#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ppc.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <windows.h>

namespace {

std::string ThreadDescription() {
  std::string name;
  PWSTR description = nullptr;
  if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description != nullptr) {
    char narrow[128];
    const int length = WideCharToMultiByte(CP_UTF8, 0, description, -1, narrow, sizeof(narrow), nullptr, nullptr);
    if (length > 1)
      name.assign(narrow, length - 1);
    LocalFree(description);
  }
  return name.empty() ? "thread " + std::to_string(GetCurrentThreadId()) : name;
}

void LogStop(const char* function, uint32_t site, const PPCRegister& r3, const PPCRegister& r4,
             const PPCRegister& r5, const PPCRegister& r6, const PPCRegister& r7) {
  // ExTerminateThread is also how every guest thread normally ends (XapiThreadStartup), so it is only info.
  if (std::string_view(function) == "ExTerminateThread")
    REXLOG_INFO("game calls {} at {:08X} (exit code {:08X}) on {}", function, site, r3.u32, ThreadDescription());
  else
    REXLOG_ERROR("game calls {} at {:08X} (r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X}) on {}", function,
                 site, r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, ThreadDescription());
  rex::FlushLogging();
}

std::string DescribeAddress(const void* address) {
  HMODULE module = nullptr;
  char text[160];
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(address), &module)) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (DWORD i = 0; i < length; i++)
      if (path[i] == L'\\' || path[i] == L'/')
        name = path + i + 1;
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::snprintf(text, sizeof(text), "%s+0x%llX", narrow,
                  (unsigned long long)(uintptr_t(address) - uintptr_t(module)));
  } else {
    std::snprintf(text, sizeof(text), "%p", address);
  }
  return text;
}

LONG WINAPI LogUnhandledException(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  REXLOG_CRITICAL("unhandled exception {:08X} at {} on {} (info {:X} {:X})", record.ExceptionCode,
                  DescribeAddress(record.ExceptionAddress), ThreadDescription(),
                  record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0,
                  record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0);
  // The handler runs on the crashing thread's stack, so this backtrace includes the faulting frames.
  void* frames[48];
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  std::string stack;
  for (USHORT i = 0; i < count; i++)
    stack += (i ? " <- " : "") + DescribeAddress(frames[i]);
  REXLOG_CRITICAL("stack: {}", stack);
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// Installed once from FrameHook, after the runtime has set up its own handlers.
void DiagnosticsInstall() {
  static bool installed = false;
  if (installed)
    return;
  installed = true;
  SetUnhandledExceptionFilter(LogUnhandledException);
}

#define PACMAN_STOP_HOOK(function, site)                                                                  \
  void StopHook_##site(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,                \
                       PPCRegister& r7) {                                                                 \
    LogStop(#function, 0x##site, r3, r4, r5, r6, r7);                                                     \
  }

// Call sites (the bl instructions), from the generated code; see pacman_config.toml.
PACMAN_STOP_HOOK(KeBugCheckEx, 82062B7C)
PACMAN_STOP_HOOK(KeBugCheckEx, 820638FC)
PACMAN_STOP_HOOK(KeBugCheckEx, 820641E4)
PACMAN_STOP_HOOK(KeBugCheckEx, 820644D4)
PACMAN_STOP_HOOK(KeBugCheck, 820B8AB4)
PACMAN_STOP_HOOK(KeBugCheck, 820B8B5C)
PACMAN_STOP_HOOK(KeBugCheck, 820BC830)
PACMAN_STOP_HOOK(KeBugCheck, 820BC998)
PACMAN_STOP_HOOK(KeBugCheck, 8215C7AC)
PACMAN_STOP_HOOK(KeBugCheck, 821BB218)
PACMAN_STOP_HOOK(XamLoaderTerminateTitle, 82061400)
PACMAN_STOP_HOOK(XamLoaderTerminateTitle, 82061574)
PACMAN_STOP_HOOK(ExTerminateThread, 820CAD08)
PACMAN_STOP_HOOK(RtlRaiseException, 820640E4)
PACMAN_STOP_HOOK(RtlRaiseException, 82064C4C)
PACMAN_STOP_HOOK(RtlRaiseException, 820CA48C)
