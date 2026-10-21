// pacman - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/input.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/overlay/achievement_notification.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

#include <windows.h> // EnumDisplaySettingsW

// XamContentGetLicenseMask returns this; 0 (the SDK default) makes the game run as the locked XBLA trial.
REXCVAR_DECLARE(uint32_t, license_mask);
// Lets the presenter present from the GPU (guest output) thread after each guest frame instead of painting on
// the UI thread at the display refresh rate.
REXCVAR_DECLARE(bool, host_present_from_non_ui_thread);
// Keep the game's 16:9 image unstretched in any window shape (pillar/letterbox bars instead of stretching).
REXCVAR_DECLARE(bool, present_letterbox);
// Lets the sampling profiler (profiler.cpp, --pacman_profile) find the UI thread.
void ProfilerNoteUiThread();
// Graphics API of the GPU plugin: vulkan (default), d3d12 or any (see OnPreSetup).
REXCVAR_DECLARE(std::string, pacman_gpu_backend);

class PacmanApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<PacmanApp>(new PacmanApp(ctx, "pacman",
        PPCImageConfig));
  }

  // Defaults so pacman.exe runs with no arguments; --game_data_root and --gpu_plugin still override them.

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (paths.game_data_root.empty())
      paths.game_data_root = FindGameData(exe_dir);
    UsePortableUserData(exe_dir, paths);
  }

  // Game data, first match: game\ next to the exe (the portable layout), the exe's own folder, or the
  // nearest private\ searching up from the exe (the repo layout, with the build in rexglue\out\build\<preset>\).
  // Empty if none holds default.xex, so the SDK asks for it.
  static std::filesystem::path FindGameData(const std::filesystem::path& exe_dir) {
    if (std::filesystem::exists(exe_dir / "game" / "default.xex"))
      return exe_dir / "game";
    if (std::filesystem::exists(exe_dir / "default.xex"))
      return exe_dir;
    for (auto dir = exe_dir; dir.has_relative_path(); dir = dir.parent_path()) {
      if (std::filesystem::exists(dir / "private" / "default.xex"))
        return dir / "private";
    }
    return {};
  }

  // Saves and the shader cache go in userdata\ next to the exe, so the folder is self-contained. Falls back to
  // the SDK default (Documents\pacman) when that folder can't be created, e.g. under Program Files. Paths set
  // with --user_data_root / --cache_root are left alone.
  static void UsePortableUserData(const std::filesystem::path& exe_dir, rex::PathConfig& paths) {
    const auto sdk_user_dir = rex::filesystem::GetUserFolder() / "pacman";
    if (paths.user_data_root != sdk_user_dir)
      return;  // set on the command line

    const auto portable_dir = exe_dir / "userdata";
    std::error_code error;
    std::filesystem::create_directories(portable_dir, error);
    if (error)
      return;

    if (paths.cache_root == sdk_user_dir / "cache")
      paths.cache_root = portable_dir / "cache";
    paths.user_data_root = portable_dir;
  }

  // GPU backend: Vulkan by default, on Windows and under Proton alike (one rendering path everywhere). ReXApp
  // would ask the plugin for "any" (D3D12 first on Windows); the plugin from our source-built SDK has both.
  // Under Proton, Vulkan goes almost straight to the native driver instead of D3D12 -> vkd3d-proton. On the PC
  // both measured the same (4K, 3x: 60 fps, ~24% GPU, 2026-10-21); --pacman_gpu_backend=d3d12 switches back.
  // GPU: the SDK leaves GPU emulation off unless a plugin is named. (An explicit empty --gpu_plugin
  // can't be told apart from "not set", so GPU emulation can't be disabled this way any more.)
  // The plugin is loaded here rather than by ReXApp right after this hook, so its settings (the render
  // resolution) exist and can be given defaults before the GPU is set up.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty())
      config.gpu_plugin = "xenos";
    if (!config.graphics) {
      const std::string backend = REXCVAR_GET(pacman_gpu_backend);
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (!config.graphics && backend != "any") {
        REXLOG_WARN("GPU backend '{}' unavailable in this build, using the default", backend);
        config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
      }
      if (config.graphics)
        ApplyDesktopResolutionScale();
      if (config.graphics && RunningUnderWine())
        ApplyWineDefaults();
    }
  }

  // Under Wine/Proton (Steam Deck) access violations and memory protection changes are very slow: a profile on
  // the Deck (2026-10-21) showed the GPU thread spending ~45% of its time re-protecting memory the game keeps
  // rewriting (per-draw vertex data), and the main thread waiting on it.
  // shared_memory_streaming_blocks (our SDK patch, patches\rexglue-sdk) stops protecting such blocks: locked
  // 60 fps on the Deck. pacman_fast_gpu_handshake is left off: with the patch it no longer helps (A/B on the
  // Deck: 0.9% vs 1.3% missed vblanks, owner felt off was slightly better) and it keeps a core busy polling.
  // Settings given on the command line or in the config file win. The plugin cvar only exists in a plugin
  // built from our patched SDK.
  static void ApplyWineDefaults() {
    if (rex::cvar::GetFlagSource("shared_memory_streaming_blocks") == rex::cvar::Source::kDefault &&
        !rex::cvar::SetFlagByName("shared_memory_streaming_blocks", "true"))
      REXLOG_INFO("Wine defaults: this GPU plugin has no shared_memory_streaming_blocks");
    REXLOG_INFO("Wine defaults: shared_memory_streaming_blocks={} pacman_fast_gpu_handshake={}",
                rex::cvar::GetFlagByName("shared_memory_streaming_blocks"),
                rex::cvar::GetFlagByName("pacman_fast_gpu_handshake"));
  }

  // Wine (and so Proton) exports wine_get_version from its ntdll; Windows never does.
  static bool RunningUnderWine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }

  // License: bit 0 marks the game as purchased, unlocking the full version instead of the trial. Runs after
  // the command line and config file are read, so a non-zero --license_mask still wins. (As with the GPU
  // default, an explicit 0 can't be told apart from "not set", so trial mode can't be forced this way.)
  void OnPostInitLogging() override {
    ProfilerNoteUiThread();
    REXLOG_INFO("Pac-Man build {} {}; profiler {}", __DATE__, __TIME__,
                rex::cvar::GetFlagByName("pacman_profile") == "true" ? "on" : "off");

    // Present like Xenia does: from the guest output thread, once per guest frame. When the presenter paints on
    // the UI thread instead (at the display refresh, 120 Hz here), the game's swap ends up waiting about one
    // host refresh per frame for that painting's GPU work, and heavy frames miss their 60 Hz vblank.
    REXCVAR_SET(host_present_from_non_ui_thread, true);

    if (!rex::cvar::HasNonDefaultValue("present_letterbox"))
      REXCVAR_SET(present_letterbox, true);
    // Start fullscreen unless --fullscreen=false (or the config file) says otherwise.
    if (rex::cvar::GetFlagSource("fullscreen") == rex::cvar::Source::kDefault)
      REXCVAR_SET(fullscreen, true);

    ApplyKeyboardControls();

    if (REXCVAR_GET(license_mask) == 0)
      REXCVAR_SET(license_mask, 1);
    REXLOG_INFO("license_mask = 0x{:X} ({})", REXCVAR_GET(license_mask),
                (REXCVAR_GET(license_mask) & 1) ? "full version" : "trial");
  }

  // Keyboard controls through the SDK's keyboard-as-controller driver (off by default). It merges into player 1
  // together with any real pad, so both work at once. Bind values are comma lists of key names; modifiers must
  // match exactly ("Up" doesn't fire while Shift is held). The SDK's own defaults spread the pad over half the
  // keyboard (arrows on the right stick, Shift+arrows on the D-pad, Q/E triggers...), so every bind is set here:
  // the game only needs movement, A, B and Start. Binds given on the command line or in the config file win.
  static void ApplyKeyboardControls() {
    struct Bind {
      const char* name;
      const char* keys;
    };
    static constexpr Bind Binds[] = {
        {"keybind_dpad_up", "Up,W"},     {"keybind_dpad_down", "Down,S"},
        {"keybind_dpad_left", "Left,A"}, {"keybind_dpad_right", "Right,D"},
        {"keybind_a", "Space,Return"},   {"keybind_b", "Escape"},
        {"keybind_start", "P"},          {"keybind_back", ""},
        {"keybind_x", ""},               {"keybind_y", ""},
        {"keybind_left_trigger", ""},    {"keybind_right_trigger", ""},
        {"keybind_left_shoulder", ""},   {"keybind_right_shoulder", ""},
        {"keybind_lstick_up", ""},       {"keybind_lstick_down", ""},
        {"keybind_lstick_left", ""},     {"keybind_lstick_right", ""},
        {"keybind_lstick_press", ""},    {"keybind_rstick_up", ""},
        {"keybind_rstick_down", ""},     {"keybind_rstick_left", ""},
        {"keybind_rstick_right", ""},    {"keybind_rstick_press", ""},
        {"keybind_guide", ""},
    };
    if (rex::cvar::GetFlagSource("mnk_mode") == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName("mnk_mode", "true");
    for (const Bind& bind : Binds) {
      if (rex::cvar::GetFlagSource(bind.name) == rex::cvar::Source::kDefault &&
          !rex::cvar::SetFlagByName(bind.name, bind.keys))
        REXLOG_WARN("keyboard controls: couldn't set {}", bind.name);
    }
    REXLOG_INFO("keyboard controls: mnk_mode={} up=[{}] a=[{}] b=[{}] start=[{}]",
                rex::cvar::GetFlagByName("mnk_mode"), rex::cvar::GetFlagByName("keybind_dpad_up"),
                rex::cvar::GetFlagByName("keybind_a"), rex::cvar::GetFlagByName("keybind_b"),
                rex::cvar::GetFlagByName("keybind_start"));
  }

  // No achievement pop-ups. The toast is the one ImGui dialog that always exists, and any dialog registers ImGui
  // as a UI drawer, which forces the presenter to paint on the UI thread at the display refresh rate (see
  // OnPostInitLogging). Achievements are still recorded; the F7 overlay still lists them.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    return nullptr;
  }

  // Render resolution from the desktop: the game renders 1280x720 (16:9), so take the largest 16:9 area that
  // fits the primary display and round its scale up to a whole number (1920x1200 -> 1.5 -> 2, rendered a bit
  // larger and scaled down). The physical display mode is used, so Windows display scaling doesn't matter.
  // --resolution_scale / --draw_resolution_scale_x / _y (or the config file) still override it. These settings
  // belong to the GPU plugin, so they only exist once it is loaded and are set by name.
  static void ApplyDesktopResolutionScale() {
    constexpr int32_t GuestWidth = 1280, GuestHeight = 720, MaxScale = 7;
    for (const char* name : {"resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y"}) {
      if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault)
        return;
    }

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) || mode.dmPelsWidth == 0 ||
        mode.dmPelsHeight == 0)
      return;

    const double fit = std::min(double(mode.dmPelsWidth) / GuestWidth, double(mode.dmPelsHeight) / GuestHeight);
    const int32_t scale = std::clamp(int32_t(std::ceil(fit - 0.01)), 1, MaxScale);
    const auto value = std::to_string(scale);
    if (!rex::cvar::SetFlagByName("draw_resolution_scale_x", value) ||
        !rex::cvar::SetFlagByName("draw_resolution_scale_y", value)) {
      REXLOG_WARN("render resolution: couldn't set the scale (GPU plugin settings missing)");
      return;
    }
    REXLOG_INFO("render resolution: {}x (desktop {}x{}, renders {}x{})", scale, mode.dmPelsWidth, mode.dmPelsHeight,
                GuestWidth * scale, GuestHeight * scale);
  }

  // F11 or Alt+Enter switches between fullscreen and windowed (the SDK has no key for it). Other keys go to
  // the SDK's key bindings (F3/F4/F7 overlays), as ReXApp::OnKeyDown does. Switching mode makes SDL send the
  // still-held key again (seen: three switches per press), so presses soon after a switch are ignored.
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    using rex::ui::VirtualKey;
    const bool toggle = e.virtual_key() == VirtualKey::kF11 ||
                        (e.virtual_key() == VirtualKey::kReturn && e.is_alt_pressed());
    if (toggle && window() != nullptr) {
      constexpr auto MinInterval = std::chrono::milliseconds(500);
      const auto now = std::chrono::steady_clock::now();
      // prev_state = auto-repeat (the SDL backend always reports repeat_count 1)
      if (!e.prev_state() && now - last_fullscreen_toggle_ >= MinInterval) {
        last_fullscreen_toggle_ = now;
        window()->SetFullscreen(!window()->IsFullscreen());
        REXLOG_INFO("fullscreen {}", window()->IsFullscreen() ? "on" : "off");
      }
      e.set_handled(true);
      return;
    }
    rex::ui::ProcessKeyEvent(e);
  }

  // Shut input down while the window still exists. ReXApp::OnDestroy destroys the window before the runtime,
  // and the runtime's shutdown then destroys the mouse/keyboard input driver, whose destructor detaches from the
  // already-freed window: heap corruption (0xC0000374) on every exit that doesn't go through the window's
  // OnClosing (e.g. quitting from the game's menu). InputSystem::Shutdown is safe to call again later.
  void OnShutdown() override {
    if (rex::Runtime* rt = runtime(); rt != nullptr && rt->input_system() != nullptr)
      rt->input_system()->Shutdown();
  }

  // The game doesn't use the mouse, so hide the cursor over the window. It comes back while the mouse moves
  // (for dragging the window or reaching the close button) and hides again after a short idle time.
  // Called on the UI thread once the window exists.
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {
    if (window() == nullptr)
      return;
    window()->SetCursorAutoHideDelayMs(1500);
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
  }

  // How the game ended, for unexplained exits: "Execution complete" alone doesn't tell a normal quit from the
  // game stopping itself (see diagnostics.cpp).
  bool OnWindowCloseRequested() override {
    close_requested_ = true;
    REXLOG_INFO("window close requested");
    return true;
  }
  void OnGuestThreadExit(rex::system::XThread*) override {
    REXLOG_WARN("game main thread ended ({})",
                close_requested_ ? "after a window close request" : "no window close request: the game stopped itself");
    rex::FlushLogging();
  }

  bool close_requested_ = false;
  std::chrono::steady_clock::time_point last_fullscreen_toggle_{};

  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
