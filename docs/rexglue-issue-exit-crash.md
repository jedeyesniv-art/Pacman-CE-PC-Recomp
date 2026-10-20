# Draft issue for ReXGlue (github.com/rexglue/rexglue-sdk/issues)

Not yet posted. Fixed in this project by commit 00b5e77 (PacmanApp::OnShutdown in rexglue/src/pacman_app.h).

---

**Title:** Heap corruption (0xC0000374) on exit when the title quits on its own: window destroyed before the input driver detaches

**Version:** SDK nightly 0.10.0.24-dev.gbd833a2 (win-amd64 prebuilt), Windows 11, D3D12, Xenos GPU plugin

**Summary**

When the game exits by itself instead of through the window being closed (in my case, choosing "back to arcade" in the game's own menu, which ends the title), the process crashes during shutdown with `STATUS_HEAP_CORRUPTION` (0xC0000374). It happens right after `Execution complete` is logged, on every such exit. The window is already gone, so nothing is visible; the only trace is a crash dump in `%LOCALAPPDATA%\CrashDumps`. Closing the window with its close button doesn't crash.

**Stack** (from the minidump; DLL frames symbolised via the nearest export, since the prebuilt has no PDBs)

```
ntdll (heap corruption detected)
rexruntime  rex::input::mnk::MnkInputDriver::<private detach helper>   (just past OnGotFocus)
rexruntime  rex::ui::WindowedAppContext::CallInUIThreadSynchronous
rexruntime  rex::input::mnk::MnkInputDriver::~MnkInputDriver
rexruntime  rex::input::DeviceInfo::~DeviceInfo / driver list teardown
rexruntime  rex::input::InputSystem::Shutdown
rexruntime  rex::Runtime::Shutdown
rexruntime  rex::Runtime::~Runtime
app         rex::ReXApp::OnDestroy        (share/rexglue/rex_app.cpp)
app         wWinMain                      (share/rexglue/windowed_app_main_sdl.cpp)
```

**Cause**

`ReXApp::OnDestroy()` (share/rexglue/rex_app.cpp) destroys the window before the runtime:

```cpp
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();   // -> InputSystem::Shutdown -> ~MnkInputDriver
```

`~MnkInputDriver()` calls `DetachFromWindow()` on its stored `attached_window_`. That pointer only gets cleared in `MnkInputDriver::OnClosing`, which fires when the window is closed by the user. When the title ends itself, `OnClosing` never runs. So by the time `runtime_.reset()` destroys the driver, `attached_window_` points at the freed window, and removing the listener from it corrupts the heap.

**Workaround (app side)**

Shut the input system down in the `OnShutdown()` hook, which `OnDestroy` calls before tearing anything down, while the window still exists. `InputSystem::Shutdown()` is idempotent, so the later call from `~Runtime` is harmless:

```cpp
void OnShutdown() override {
  if (rex::Runtime* rt = runtime(); rt && rt->input_system())
    rt->input_system()->Shutdown();
}
```

With this, quitting from the game's menu exits cleanly (no dump), and closing the window still works.

**Possible fixes in the SDK**
- In `ReXApp::OnDestroy`, shut down the input system (or the runtime) before `window_.reset()`; or
- have `MnkInputDriver` detach when the window is destroyed (a window-destroyed event, or `InputSystem` detaching drivers from its window in `Shutdown` before the window goes away), rather than relying on `OnClosing`.
