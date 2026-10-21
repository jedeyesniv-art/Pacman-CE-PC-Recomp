// pacman - ReXGlue Recompiled Project

#include "generated/default/pacman_init.h"

#include "pacman_app.h"

REXCVAR_DEFINE_STRING(pacman_gpu_backend, "vulkan", "Pac-Man",
                      "Graphics API: vulkan (falls back to the plugin's default if it has no Vulkan), d3d12, or any "
                      "(the plugin's default: D3D12 on Windows)")
    .allowed({"any", "d3d12", "vulkan"});

// The game's Achievements menu item: entry of sub_820C9CC0, its wrapper around XamShowAchievementsUI (a stub in
// the SDK). Opens the SDK's achievements overlay instead (PacmanApp::RequestAchievementsOverlay).
void AchievementsUiHook() {
  if (PacmanApp* app = PacmanApp::instance())
    app->RequestAchievementsOverlay(true);
}

// Every keystroke the game reads (autodemo.cpp's hook): B closes the overlay opened above.
void AchievementsKeystroke(uint16_t virtual_key, uint16_t flags) {
  constexpr uint16_t VkPadB = 0x5801, KeyDown = 0x0001;
  if (virtual_key == VkPadB && (flags & KeyDown))
    if (PacmanApp* app = PacmanApp::instance(); app != nullptr && app->achievements_open())
      app->RequestAchievementsOverlay(false);
}

REX_DEFINE_APP(pacman, PacmanApp::Create)
