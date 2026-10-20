// pacman - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/filesystem.h>
#include <rex/rex_app.h>

#include <filesystem>

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

  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
