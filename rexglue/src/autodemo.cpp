// Auto-demo for repeatable measurements (--pacman_autodemo_seconds=N, off by default): N seconds after launch
// (12 works; 6 lands while the game is still loading), player 1's A button is held for a moment, as the owner does
// by hand to get from the start menu to the gameplay demo. Injected into the state the game reads (no real input,
// no window focus needed).
//
// Hooked after the game's controller poll (sub_820A5D28 area: bl XInputGetState sub_820C9810 in a loop over
// the four users, user index in r28, XINPUT_STATE on the stack at r1+0x50: packet number u32, buttons u16,
// big-endian; r3 = result, 0 = connected).

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <chrono>
#include <cstdint>

#include <stdlib.h>  // _byteswap_*

REXCVAR_DEFINE_DOUBLE(pacman_autodemo_seconds, 0.0, "Pac-Man",
                      "Press A once this many seconds after launch (0 = off; 12 reaches the gameplay demo; for tests)");

namespace {
constexpr uint16_t ButtonA = 0x1000;
constexpr auto PressLength = std::chrono::milliseconds(150);
const std::chrono::steady_clock::time_point g_launch = std::chrono::steady_clock::now();  // static init = launch
}  // namespace

void AutoDemoInputHook(PPCRegister& r1, PPCRegister& r3, PPCRegister& r28) {
  const double delay = REXCVAR_GET(pacman_autodemo_seconds);
  if (delay <= 0 || r28.u32 != 0 || r3.u32 != 0)
    return;
  using Clock = std::chrono::steady_clock;
  static bool logged = false;
  const auto pressAt = g_launch + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delay));
  const auto now = Clock::now();
  if (now < pressAt || now >= pressAt + PressLength)
    return;

  rex::Runtime* runtime = rex::Runtime::instance();
  if (runtime == nullptr)
    return;
  uint8_t* state = runtime->virtual_membase() + r1.u32 + 0x50;
  auto* packet = reinterpret_cast<uint32_t*>(state);
  auto* buttons = reinterpret_cast<uint16_t*>(state + 4);
  *buttons = _byteswap_ushort(uint16_t(_byteswap_ushort(*buttons) | ButtonA));
  *packet = _byteswap_ulong(_byteswap_ulong(*packet) + 1);
  if (!logged) {
    logged = true;
    REXLOG_INFO("auto-demo: pressing A ({:.1f} s after launch)", delay);
  }
}

// The menus read button presses as keystroke events instead (XamInputGetKeystrokeEx, wrapped by sub_820C9820;
// hooked on its return: r3 = result, r5 = the XINPUT_KEYSTROKE the game passed). Once the press time has come
// and the queue is empty, report A pressed, then released on the next read.
void AutoDemoKeystrokeHook(PPCRegister& r3, PPCRegister& r5) {
  constexpr uint32_t ErrorEmpty = 0x10D2;
  constexpr uint16_t VkPadA = 0x5800, KeyDown = 0x0001, KeyUp = 0x0002;
  static int sent = 0;  // 0 nothing yet, 1 key down sent, 2 key up sent
  const double delay = REXCVAR_GET(pacman_autodemo_seconds);
  if (delay <= 0 || sent == 2 || r3.u32 != ErrorEmpty || r5.u32 == 0)
    return;
  const auto pressAt = g_launch + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(delay));
  if (std::chrono::steady_clock::now() < pressAt)
    return;
  rex::Runtime* runtime = rex::Runtime::instance();
  if (runtime == nullptr)
    return;
  uint8_t* keystroke = runtime->virtual_membase() + r5.u32;  // VirtualKey, Unicode, Flags (u16), UserIndex, HidCode
  *reinterpret_cast<uint16_t*>(keystroke + 0) = _byteswap_ushort(VkPadA);
  *reinterpret_cast<uint16_t*>(keystroke + 2) = 0;
  *reinterpret_cast<uint16_t*>(keystroke + 4) = _byteswap_ushort(sent == 0 ? KeyDown : KeyUp);
  keystroke[6] = 0;
  keystroke[7] = 0;
  r3.u64 = 0;
  if (++sent == 1)
    REXLOG_INFO("auto-demo: A keystroke ({:.1f} s after launch)", delay);
}
