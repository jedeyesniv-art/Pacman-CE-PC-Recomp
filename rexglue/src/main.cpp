// pacman - ReXGlue Recompiled Project

#include "generated/default/pacman_init.h"

#include "pacman_app.h"

REXCVAR_DEFINE_STRING(pacman_gpu_backend, "vulkan", "Pac-Man",
                      "Graphics API: vulkan (falls back to the plugin's default if it has no Vulkan), d3d12, or any "
                      "(the plugin's default: D3D12 on Windows)")
    .allowed({"any", "d3d12", "vulkan"});

REX_DEFINE_APP(pacman, PacmanApp::Create)
