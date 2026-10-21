// Random maze order for Championship mode - a secret bonus mode, toggled on the title screen.
//
// Toggle: R3 (keyboard R) while the title screen shows; the "PAC-MAN Championship Edition" logo turns red while it is
// on. --pacman_random_mazes=true turns it on at launch. A run is random if the toggle was on when it started.
//
// Maze order: each mode's settings come from a Shift-JIS CSV in data\ (Championship = gamedata1.bin), parsed when a
// mode starts into a tree of keyed entries. The stage lists become three sections, p_map_l.csv / p_map_m.csv /
// p_map_r.csv (left half, centre divider, right half); each child is one stage row: the maze name (looked up as
// "<name>.csv", an embedded maze file) and its numbers. The game keeps one stage counter per half (0x821EDD80 +
// 0x6B82 / 0x6B84 / 0x6B86) and asks for a stage's data through two functions that both reduce the index modulo
// the section's child count: sub_8213E948 (maze file name) and sub_82140210 (number). The name and the numbers
// drive different parts of the maze (owner play-test: changing only one desyncs dots and walls), so the index is
// remapped in both, which keeps every stage whole. Each half gets its own permutation of stages 2..N (stage 1
// stays), drawn anew at every run start (sub_8213F438 resets the counters). Other modes are left alone.
//
// Scores: random runs record no high scores. The game keeps its own copy of the save data and commits it with
// sub_820A8CE8(data, size) (memcpy into the live save buffer at *0x821EB084, then a save if it changed); that
// commit is skipped during a random run, and when the title screen shows again the game's copy is restored from
// the untouched live buffer, so a random score can't reach a later save or the HI display.
//
// Title screen: the logo (1042x327 texture) and the menu's selection highlight (400x61) are drawn by the UI layer
// with pixel shader F8BB1F8E90AB7819 (texture x pixel constant c1); the SDK draw filter (patch 0008) multiplies
// their c1 by red while the toggle is on. Those draws also tell us the title screen is showing: R3 counts while
// either was drawn in the last 300 ms (the logo animates off screen now and then; the highlight stays), and the
// score restore waits for the logo itself (only the title screen has it; the highlight may appear in other menus).

#include <rex/cvar.h>
#include <rex/graphics/draw_filter.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <stdlib.h>  // _byteswap_ulong

REXCVAR_DEFINE_BOOL(pacman_random_mazes, false, "Pac-Man",
                    "Random Championship maze order on from launch (also toggled with R3 / keyboard R on the title "
                    "screen; random runs record no high scores)");

namespace {

// Host pointer for a guest address (TranslateVirtual: the 0xE0000000+ heaps, where the data tree lives, are
// mapped with an extra offset, so membase + address would be wrong there).
uint8_t* Guest(uint32_t address) {
  return rex::Runtime::instance()->memory()->TranslateVirtual<uint8_t*>(address);
}

uint32_t Load32(uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Guest(address), 4);
  return _byteswap_ulong(value);
}

const char* Key(uint32_t entry) {
  return reinterpret_cast<const char*>(Guest(entry + 0x30));
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<bool> g_armed{false};           // the toggle
std::atomic<bool> g_run_random{false};      // latched at run start
std::atomic<bool> g_scores_blocked{false};  // a random Championship run happened since the title screen
std::atomic<int64_t> g_logo_drawn_ms{-1000000};   // title logo (only on the title screen)
std::atomic<int64_t> g_title_drawn_ms{-1000000};  // logo or the menu highlight (stays while the logo animates away)
std::atomic<int64_t> g_blocked_ms{0};             // when g_scores_blocked was set

// ---- maze order ----

struct HalfOrder {
  uint32_t section = 0;  // the section the order was made for
  bool championship = false;
  uint32_t count = 0;
  bool reshuffle = true;
  std::vector<uint32_t> stage;  // stage[i] = row the game gets when it asks for row i
};
std::mutex g_mutex;
HalfOrder g_halves[2];  // left, right
std::mt19937 g_random{std::random_device{}()};

// 0 = p_map_l.csv, 1 = p_map_r.csv, -1 = any other section.
int Half(uint32_t section) {
  if (std::strncmp(Key(section), "p_map_l.csv", 12) == 0)
    return 0;
  if (std::strncmp(Key(section), "p_map_r.csv", 12) == 0)
    return 1;
  return -1;
}

// The sections are the same in every mode; Championship's stage list (gamedata1.bin) is the only one that starts
// m_i_map00, 01, 02, 03, 11. Children: first at section+0x14, next at +0x18.
bool IsChampionship(uint32_t section) {
  static constexpr const char* Expected[] = {"m_i_map00", "m_i_map01", "m_i_map02", "m_i_map03", "m_i_map11"};
  uint32_t child = Load32(section + 0x14);
  for (const char* expected : Expected) {
    if (child == 0 || std::strncmp(Key(child), expected, 16) != 0)
      return false;
    child = Load32(child + 0x18);
  }
  return true;
}

uint32_t Remap(uint32_t section, uint32_t count, uint32_t index) {
  if (!g_run_random.load(std::memory_order_relaxed) || count < 3 || section == 0)
    return index;
  const int half = Half(section);
  if (half < 0)
    return index;
  std::lock_guard lock(g_mutex);
  HalfOrder& order = g_halves[half];
  if (order.section != section) {
    order.section = section;
    order.championship = IsChampionship(section);
    order.reshuffle = true;
    if (order.championship && !g_scores_blocked.load()) {
      g_blocked_ms.store(NowMs());
      g_scores_blocked.store(true);
      REXLOG_INFO("random mazes: random Championship run - high scores are not recorded");
    }
  }
  if (!order.championship)
    return index;
  if (order.reshuffle || order.count != count) {
    order.count = count;
    order.reshuffle = false;
    order.stage.resize(count);
    std::iota(order.stage.begin(), order.stage.end(), 0u);
    std::shuffle(order.stage.begin() + 1, order.stage.end(), g_random);
    std::string text;
    for (uint32_t row : order.stage)
      text += " " + std::to_string(row + 1);
    REXLOG_INFO("random mazes: {} half order:{}", half == 0 ? "left" : "right", text);
  }
  return order.stage[index % count];
}

// ---- scores ----

uint32_t g_save_data = 0, g_save_size = 0;  // the game's own copy of the save data (seen at its first commit)

void RestoreScores() {
  const uint32_t live = Load32(0x821EB084);
  if (g_save_data != 0 && live != 0 && g_save_size != 0)
    std::memcpy(Guest(g_save_data), Guest(live), g_save_size);
  REXLOG_INFO("random mazes: back on the title screen - high scores restored from the save");
}

// ---- title logo ----

constexpr uint32_t FetchConstant0 = 0x4800;  // SHADER_CONSTANT_FETCH_00_0 (texture fetch constant 0, 6 dwords)
constexpr uint32_t PixelConstant1 = 0x4404;  // SHADER_CONSTANT_256_X + 4 = pixel c1 (x, y, z, w)

// Texture fetch constant 0 size (2D: width - 1 in bits 0-12, height - 1 in bits 13-25).
bool TextureSize(const uint32_t* registers, uint32_t width, uint32_t height) {
  const uint32_t size = registers[FetchConstant0 + 2];
  return (size & 0x1FFF) == width - 1 && ((size >> 13) & 0x1FFF) == height - 1;
}

uint32_t TitleFilter(const rex::graphics::DrawFilterInfo& info, rex::graphics::DrawRegisterWrite* writes,
                     uint32_t max_writes) {
  if (info.pixel_shader_hash != 0xF8BB1F8E90AB7819ull)
    return 0;
  const bool logo = TextureSize(info.registers, 1042, 327);
  if (!logo && !TextureSize(info.registers, 400, 61))  // the menu's selection highlight
    return 0;
  const int64_t now = NowMs();
  g_title_drawn_ms.store(now, std::memory_order_relaxed);
  if (logo)
    g_logo_drawn_ms.store(now, std::memory_order_relaxed);
  if (!g_armed.load(std::memory_order_relaxed) || max_writes < 2)
    return 0;
  // Gold or white x (1, 0.15, 0.15) = red.
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t index = PixelConstant1 + 1 + i;  // green, blue
    writes[i] = {index, std::bit_cast<uint32_t>(std::bit_cast<float>(info.registers[index]) * 0.15f)};
  }
  return 2;
}

}  // namespace

// sub_8213E948 before divwu r9,r31,r10: r3 = section, r10 = child count, r31 = stage index.
void StageNameIndexHook(PPCRegister& r3, PPCRegister& r10, PPCRegister& r31) {
  r31.u64 = Remap(r3.u32, r10.u32, r31.u32);
}

// sub_82140210 before divwu r10,r30,r11: r3 = section, r11 = child count, r30 = stage index.
void StageValueIndexHook(PPCRegister& r3, PPCRegister& r11, PPCRegister& r30) {
  r30.u64 = Remap(r3.u32, r11.u32, r30.u32);
}

// sub_8213F438 resetting the stage counters: a run starts. It is random if the toggle is on now; either way the
// next lookups re-check the mode (a new data tree can reuse the same address) and draw a new order.
void StageRunStartHook() {
  g_run_random.store(g_armed.load(std::memory_order_relaxed), std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  for (HalfOrder& order : g_halves) {
    order.section = 0;
    order.reshuffle = true;
  }
}

// Entry of sub_820A8CE8(r3 = the game's save data, r4 = size): skipped (returns) during a random run.
bool SaveCommitHook(PPCRegister& r3, PPCRegister& r4) {
  if (r4.u32 != 0 && r4.u32 <= 0x1000) {
    g_save_data = r3.u32;
    g_save_size = r4.u32;
  }
  if (!g_scores_blocked.load(std::memory_order_relaxed))
    return false;
  REXLOG_INFO("random mazes: save skipped (random run)");
  return true;
}

// Every keystroke read by the game (autodemo.cpp's hook on XamInputGetKeystrokeEx; virtual_key 0 = none). The
// title screen reads them too, so this is also where coming back from a random run is noticed.
void RandomMazesKeystroke(uint16_t virtual_key, uint16_t flags) {
  constexpr uint16_t VkPadRThumbPress = 0x5817, KeyDown = 0x0001, Repeat = 0x0004;
  const int64_t now = NowMs();
  // Back on the title screen after a random run: the logo was drawn since the run began.
  if (g_scores_blocked.load(std::memory_order_relaxed) &&
      g_logo_drawn_ms.load(std::memory_order_relaxed) > g_blocked_ms.load(std::memory_order_relaxed) &&
      now - g_logo_drawn_ms.load(std::memory_order_relaxed) < 300) {
    RestoreScores();
    g_scores_blocked.store(false);
  }
  if (virtual_key == VkPadRThumbPress && (flags & KeyDown) && !(flags & Repeat) &&
      now - g_title_drawn_ms.load(std::memory_order_relaxed) < 300) {
    const bool armed = !g_armed.load();
    g_armed.store(armed);
    REXLOG_INFO("random mazes: {} (title screen R3)", armed ? "on" : "off");
  }
}

// Called at startup (pacman_app.h).
void RandomMazesSetup() {
  g_armed.store(REXCVAR_GET(pacman_random_mazes));
  rex::graphics::SetDrawRegisterFilter(TitleFilter);
  if (g_armed.load())
    REXLOG_INFO("random mazes: on (--pacman_random_mazes)");
}
