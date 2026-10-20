// Playfield position (hooked in pacman_config.toml).
//
// The game centres its world (maze and the pulsing neon frames) 40 px below the middle of the 720p screen,
// so the bottom of the outer frame is cut off at its larger pulse sizes. These hooks move the whole image
// (HUD included) up by shifting the game's two view matrices in clip space, so what was below the screen
// edge is drawn rather than clipped.

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

REXCVAR_DEFINE_INT32(pacman_playfield_offset, 40, "Pac-Man",
                     "Move the picture up by this many pixels (at 720p; 0 = the game's original layout)");

namespace {

// Sprite matrix: the 2D view for the maze, characters and HUD (rows = vertex shader constants c0-c3).
constexpr uint32_t SpriteMatrix = 0x821EDC30;
// Startup camera's perspective projection (row-vector convention); used for the ghost-house clipping.
constexpr uint32_t ProjectionMatrix = 0x821E5950;
constexpr double ScreenHeight = 720.0;

float LoadFloat(uint8_t* base, uint32_t address) {
  const uint32_t bits = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + address));
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  *reinterpret_cast<uint32_t*>(base + address) = _byteswap_ulong(bits);
}

// Clip-space shift in NDC (+ = up), or 0 when off.
float ShiftNdc() {
  return float(2.0 * REXCVAR_GET(pacman_playfield_offset) / ScreenHeight);
}

}  // namespace

// After sub_8213B1C0 stores the sprite matrix. oPos.y = dot(c1, v) and oPos.w = dot(c3, v), so adding
// ty * c3 to c1 moves the image by ty in NDC.
void SpriteMatrixHook() {
  const float ty = ShiftNdc();
  if (ty == 0)
    return;
  auto* base = static_cast<uint8_t*>(rex::Runtime::instance()->virtual_membase());
  for (int i = 0; i < 4; i++) {
    const uint32_t y = SpriteMatrix + 16 + i * 4;
    StoreFloat(base, y, LoadFloat(base, y) + ty * LoadFloat(base, SpriteMatrix + 48 + i * 4));
  }
}

// After sub_820607E0 builds the startup camera's projection (bl sub_8207FCD8). With row vectors (v * M) the
// y column gains ty times the w column.
void ProjectionHook() {
  const float ty = ShiftNdc();
  if (ty == 0)
    return;
  auto* base = static_cast<uint8_t*>(rex::Runtime::instance()->virtual_membase());
  for (int row = 0; row < 4; row++) {
    const uint32_t y = ProjectionMatrix + (row * 4 + 1) * 4;
    StoreFloat(base, y, LoadFloat(base, y) + ty * LoadFloat(base, ProjectionMatrix + (row * 4 + 3) * 4));
  }
}
