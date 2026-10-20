// Frame-time monitor: the game advances one step per frame it presents (VdSwap), timed by the 60 Hz guest
// vblank, so any frame that takes longer than its slot slows the game down while music keeps real time.
// FrameHook (hooked on the game's only VdSwap call, see pacman_config.toml) logs a summary every 10 s:
// frames, frames that missed their slot, the worst frame, and how far game time has fallen behind.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

REXCVAR_DEFINE_BOOL(pacman_frame_stats, true, "Pac-Man",
                    "Log a frame-time summary every 10 s (frames, slow frames, game time lost)");

namespace {

using Clock = std::chrono::steady_clock;

constexpr double GuestFrameSeconds = 1.0 / 60.0;
constexpr double SlowFrameSeconds = 0.020;  // a frame this long has missed its 16.7 ms slot
constexpr auto ReportInterval = std::chrono::seconds(10);

struct Window {
  Clock::time_point start;
  uint32_t frames = 0;
  uint32_t slowFrames = 0;
  double worstSeconds = 0;
};

}  // namespace

void FrameHook() {
  if (!REXCVAR_GET(pacman_frame_stats))
    return;

  static Clock::time_point last;
  static Window window;
  static double totalLostSeconds = 0;

  const auto now = Clock::now();
  if (last == Clock::time_point{}) {
    last = now;
    window.start = now;
    return;
  }

  const double frameSeconds = std::chrono::duration<double>(now - last).count();
  last = now;
  window.frames++;
  if (frameSeconds > SlowFrameSeconds)
    window.slowFrames++;
  window.worstSeconds = std::max(window.worstSeconds, frameSeconds);

  if (now - window.start >= ReportInterval) {
    const double elapsed = std::chrono::duration<double>(now - window.start).count();
    // Game time advances one guest frame per presented frame; anything beyond that is time lost.
    const double lost = std::max(0.0, elapsed - window.frames * GuestFrameSeconds);
    totalLostSeconds += lost;
    REXLOG_INFO("frames: {} in {:.1f} s ({:.1f} fps), {} over {:.0f} ms, worst {:.1f} ms; game behind by "
                "{:.0f} ms this window, {:.1f} s total",
                window.frames, elapsed, window.frames / elapsed, window.slowFrames, SlowFrameSeconds * 1000,
                window.worstSeconds * 1000, lost * 1000, totalLostSeconds);
    window = Window{now};
  }
}
