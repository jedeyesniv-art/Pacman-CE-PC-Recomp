// pacman - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/input.h>
#include <rex/ui/overlay/achievement_notification.h>

#include <cstdint>
#include <filesystem>

// XamContentGetLicenseMask returns this; 0 (the SDK default) makes the game run as the locked XBLA trial.
REXCVAR_DECLARE(uint32_t, license_mask);
// Lets the presenter present from the GPU (guest output) thread after each guest frame instead of painting on
// the UI thread at the display refresh rate.
REXCVAR_DECLARE(bool, host_present_from_non_ui_thread);

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

  // GPU: the SDK leaves GPU emulation off unless a plugin is named. (An explicit empty --gpu_plugin
  // can't be told apart from "not set", so GPU emulation can't be disabled this way any more.)
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty())
      config.gpu_plugin = "xenos";
  }

  // License: bit 0 marks the game as purchased, unlocking the full version instead of the trial. Runs after
  // the command line and config file are read, so a non-zero --license_mask still wins. (As with the GPU
  // default, an explicit 0 can't be told apart from "not set", so trial mode can't be forced this way.)
  void OnPostInitLogging() override {
    // Present like Xenia does: from the guest output thread, once per guest frame. When the presenter paints on
    // the UI thread instead (at the display refresh, 120 Hz here), the game's swap ends up waiting about one
    // host refresh per frame for that painting's GPU work, and heavy frames miss their 60 Hz vblank.
    REXCVAR_SET(host_present_from_non_ui_thread, true);

    if (REXCVAR_GET(license_mask) == 0)
      REXCVAR_SET(license_mask, 1);
    REXLOG_INFO("license_mask = 0x{:X} ({})", REXCVAR_GET(license_mask),
                (REXCVAR_GET(license_mask) & 1) ? "full version" : "trial");
  }

  // No achievement pop-ups. The toast is the one ImGui dialog that always exists, and any dialog registers ImGui
  // as a UI drawer, which forces the presenter to paint on the UI thread at the display refresh rate (see
  // OnPostInitLogging). Achievements are still recorded; the F7 overlay still lists them.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    return nullptr;
  }

  // Shut input down while the window still exists. ReXApp::OnDestroy destroys the window before the runtime,
  // and the runtime's shutdown then destroys the mouse/keyboard input driver, whose destructor detaches from the
  // already-freed window: heap corruption (0xC0000374) on every exit that doesn't go through the window's
  // OnClosing (e.g. quitting from the game's menu). InputSystem::Shutdown is safe to call again later.
  void OnShutdown() override {
    if (rex::Runtime* rt = runtime(); rt != nullptr && rt->input_system() != nullptr)
      rt->input_system()->Shutdown();
  }

  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
