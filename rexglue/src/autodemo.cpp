// Auto-demo for repeatable measurements and tests: scripted button presses on player 1's pad, injected into the
// state the game reads (no real input, no window focus needed).
//   --pacman_autodemo_seconds=N   press A N seconds after launch (12 reaches the gameplay demo)
//   --pacman_autodemo_keys=LIST   press buttons at set times, e.g. "10:A,14:A,17:DOWN,19:A" (seconds after launch;
//                                 A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS)
//
// Hooked after the game's controller poll (sub_820A5D28 area: bl XInputGetState sub_820C9810 in a loop over
// the four users, user index in r28, XINPUT_STATE on the stack at r1+0x50: packet number u32, buttons u16,
// big-endian; r3 = result, 0 = connected), and on the menus' keystroke reads (see AutoDemoKeystrokeHook).

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <stdlib.h>  // _byteswap_*

REXCVAR_DEFINE_DOUBLE(pacman_autodemo_seconds, 0.0, "Pac-Man",
                      "Press A once this many seconds after launch (0 = off; 12 reaches the gameplay demo; for tests)");
REXCVAR_DEFINE_STRING(pacman_autodemo_keys, "", "Pac-Man",
                      "Scripted presses for tests: seconds:button list, e.g. 10:A,14:A,17:DOWN (buttons A B X Y "
                      "START BACK UP DOWN LEFT RIGHT LB RB LS RS)");

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto PressLength = std::chrono::milliseconds(150);
const Clock::time_point g_launch = Clock::now();  // static init = launch

struct Press {
  Clock::time_point at;
  uint16_t button;    // XINPUT_GAMEPAD_* bit
  uint16_t key;       // VK_PAD_* for keystrokes
  std::string name;
  int sent = 0;       // keystrokes: 0 nothing yet, 1 key down sent, 2 key up sent
};

struct ButtonName {
  const char* name;
  uint16_t button, key;
};
constexpr ButtonName Buttons[] = {
    {"A", 0x1000, 0x5800},     {"B", 0x2000, 0x5801},    {"X", 0x4000, 0x5802},     {"Y", 0x8000, 0x5803},
    {"RB", 0x0200, 0x5804},    {"LB", 0x0100, 0x5805},   {"UP", 0x0001, 0x5810},    {"DOWN", 0x0002, 0x5811},
    {"LEFT", 0x0004, 0x5812},  {"RIGHT", 0x0008, 0x5813}, {"START", 0x0010, 0x5814}, {"BACK", 0x0020, 0x5815},
    {"LS", 0x0040, 0x5816},    {"RS", 0x0080, 0x5817},
};

std::once_flag g_parsed;
std::vector<Press> g_presses;

Clock::time_point After(double seconds) {
  return g_launch + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

std::vector<Press>& Presses() {
  std::call_once(g_parsed, [] {
    if (const double delay = REXCVAR_GET(pacman_autodemo_seconds); delay > 0)
      g_presses.push_back({After(delay), 0x1000, 0x5800, "A"});
    std::stringstream list(REXCVAR_GET(pacman_autodemo_keys));
    std::string item;
    while (std::getline(list, item, ',')) {
      const size_t colon = item.find(':');
      if (colon == std::string::npos)
        continue;
      const double seconds = std::atof(item.substr(0, colon).c_str());
      const std::string name = item.substr(colon + 1);
      bool known = false;
      for (const ButtonName& button : Buttons) {
        if (name == button.name) {
          g_presses.push_back({After(seconds), button.button, button.key, name});
          known = true;
        }
      }
      if (!known)
        REXLOG_WARN("auto-demo: unknown button '{}'", name);
    }
  });
  return g_presses;
}
}  // namespace

void AutoDemoInputHook(PPCRegister& r1, PPCRegister& r3, PPCRegister& r28) {
  if (r28.u32 != 0 || r3.u32 != 0)
    return;
  std::vector<Press>& presses = Presses();
  if (presses.empty())
    return;
  const auto now = Clock::now();
  uint16_t held = 0;
  for (const Press& press : presses) {
    if (now >= press.at && now < press.at + PressLength)
      held |= press.button;
  }
  if (held == 0)
    return;
  rex::Runtime* runtime = rex::Runtime::instance();
  if (runtime == nullptr)
    return;
  uint8_t* state = runtime->virtual_membase() + r1.u32 + 0x50;
  auto* packet = reinterpret_cast<uint32_t*>(state);
  auto* buttons = reinterpret_cast<uint16_t*>(state + 4);
  *buttons = _byteswap_ushort(uint16_t(_byteswap_ushort(*buttons) | held));
  *packet = _byteswap_ulong(_byteswap_ulong(*packet) + 1);
}

namespace {
// When a scripted press is due and the queue is empty, report it pressed, then released on the next read.
void InjectKeystroke(PPCRegister& r3, PPCRegister& r5) {
  constexpr uint32_t ErrorEmpty = 0x10D2;
  constexpr uint16_t KeyDown = 0x0001, KeyUp = 0x0002;
  if (r3.u32 != ErrorEmpty || r5.u32 == 0)
    return;
  std::vector<Press>& presses = Presses();
  const auto now = Clock::now();
  for (Press& press : presses) {
    if (press.sent == 2 || now < press.at)
      continue;
    rex::Runtime* runtime = rex::Runtime::instance();
    if (runtime == nullptr)
      return;
    uint8_t* keystroke = runtime->virtual_membase() + r5.u32;  // VirtualKey, Unicode, Flags (u16), UserIndex, HidCode
    *reinterpret_cast<uint16_t*>(keystroke + 0) = _byteswap_ushort(press.key);
    *reinterpret_cast<uint16_t*>(keystroke + 2) = 0;
    *reinterpret_cast<uint16_t*>(keystroke + 4) = _byteswap_ushort(press.sent == 0 ? KeyDown : KeyUp);
    keystroke[6] = 0;
    keystroke[7] = 0;
    r3.u64 = 0;
    if (++press.sent == 1)
      REXLOG_INFO("auto-demo: {} ({:.1f} s after launch)", press.name,
                  std::chrono::duration<double>(now - g_launch).count());
    return;
  }
}
}  // namespace

void RandomMazesKeystroke(uint16_t virtual_key, uint16_t flags);  // random_mazes.cpp

// The menus read button presses as keystroke events instead (XamInputGetKeystrokeEx, wrapped by sub_820C9820;
// hooked on its return: r3 = result, 0 = a keystroke; r5 = the XINPUT_KEYSTROKE the game passed: VirtualKey,
// Unicode, Flags (u16, big-endian), UserIndex, HidCode).
void AutoDemoKeystrokeHook(PPCRegister& r3, PPCRegister& r5) {
  InjectKeystroke(r3, r5);
  uint16_t virtual_key = 0, flags = 0;
  if (r3.u32 == 0 && r5.u32 != 0) {
    const uint8_t* keystroke = rex::Runtime::instance()->virtual_membase() + r5.u32;
    virtual_key = _byteswap_ushort(*reinterpret_cast<const uint16_t*>(keystroke));
    flags = _byteswap_ushort(*reinterpret_cast<const uint16_t*>(keystroke + 4));
  }
  RandomMazesKeystroke(virtual_key, flags);
}
