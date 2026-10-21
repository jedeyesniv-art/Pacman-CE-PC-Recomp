// pacman - ReXGlue Recompiled Project

#include "generated/default/pacman_init.h"

#include "pacman_app.h"

REXCVAR_DEFINE_STRING(pacman_gpu_backend, "any", "Pac-Man",
                      "Graphics API: any (D3D12 if available; Vulkan under Wine/Proton), d3d12, or vulkan (needs a "
                      "plugin built with Vulkan)")
    .allowed({"any", "d3d12", "vulkan"});

REX_DEFINE_APP(pacman, PacmanApp::Create)
