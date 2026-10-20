// pacman - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <cstdint>
#include <filesystem>

// XamContentGetLicenseMask returns this; 0 (the SDK default) makes the game run as the locked XBLA trial.
REXCVAR_DECLARE(uint32_t, license_mask);

class PacmanApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<PacmanApp>(new PacmanApp(ctx, "pacman",
        PPCImageConfig));
  }

  // Defaults so pacman.exe runs with no arguments; --game_data_root and --gpu_plugin still override them.

  // Game data: the nearest private\ folder holding default.xex, searching up from the exe (the repo layout
  // puts the build in rexglue\out\build\<preset>\). Left empty if not found, so the SDK asks for it.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty())
      return;
    for (auto dir = rex::filesystem::GetExecutableFolder(); dir.has_relative_path(); dir = dir.parent_path()) {
      if (std::filesystem::exists(dir / "private" / "default.xex")) {
        paths.game_data_root = dir / "private";
        return;
      }
    }
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
    if (REXCVAR_GET(license_mask) == 0)
      REXCVAR_SET(license_mask, 1);
    REXLOG_INFO("license_mask = 0x{:X} ({})", REXCVAR_GET(license_mask),
                (REXCVAR_GET(license_mask) & 1) ? "full version" : "trial");
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
