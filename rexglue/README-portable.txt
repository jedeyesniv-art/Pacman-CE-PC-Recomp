Pac-Man Championship Edition - native PC build, version 1.0.0
=============================================================

Setup
  Put your own copy of the game in the game\ folder, either way:
    - the game's package file as dumped from your console (one file with a long hexadecimal name, often
      inside 58410877\000D0000\ folders - copy those along, or just the file), or
    - the unpacked files: game\default.xex, game\media\..., game\data\...
  Then run pacman.exe. No installation is needed; the whole folder can be moved or copied.

Files
  pacman.exe, *.dll   the game and its runtime (Visual C++ runtime included)
  game\               your game files (not included)
  userdata\           created on first run: saves and the shader cache. Delete it to start fresh.
  userdata\save-backups\  automatic copies of your saves, made at startup whenever they changed (the
                      newest 10 are kept). To restore one, copy its contents back into userdata\.
  pacman.toml         optional ReXGlue settings file, read at startup if present (not created automatically)
  licenses\           licences: this port (MIT-0) and the third-party software it uses
                      (start with THIRD-PARTY-NOTICES.txt)

Requirements
  64-bit Windows 10 or 11 with a Vulkan-capable GPU (Direct3D 12 also works, see the options below),
  or a Steam Deck: add pacman.exe as a non-Steam game and run it with Proton. No launch options are needed.

Controls
  An Xbox controller works as is. The keyboard works too, at the same time as a controller:
    Arrow keys or W A S D   move (D-pad)
    Space or Enter          A
    Escape                  B
    P                       Start (pause)
  To change a key, set its keybind_* option, e.g. --keybind_start=P,Tab (a comma list of key names), or turn
  the keyboard off with --mnk_mode=false.

Random mazes (bonus mode)
  On the title screen, press R3 (click the right stick; R on the keyboard): the logo turns red, and
  Championship Mode now plays its mazes in a random order, a new one every game (the first maze stays).
  Press it again to switch back. Random games record no high scores. --pacman_random_mazes=true starts with
  it on.

Display
  The game renders at your main monitor's resolution: its 1280x720 picture is scaled up by a whole number
  to fill the screen (3x on a 4K screen, 2x on 1440p, 16:10 and ultrawide screens). On a screen or window
  that isn't 16:9, the picture is never stretched; black bars fill the sides or the top and bottom.
  Lower the scale if your GPU struggles, e.g. --resolution_scale=1 for the original 720p.
  The game starts fullscreen. F11 or Alt+Enter switches between fullscreen and a window.
  The picture is moved up 40 pixels (at 720p) from the original, which sits too low and cuts off the bottom
  of the pulsing frame around the maze. --pacman_playfield_offset=0 restores the original position.

Options (on the command line, e.g. pacman.exe --fullscreen=false)
  --fullscreen=false                         start in a window
  --pacman_gpu_backend=vulkan|d3d12          graphics API (default: Vulkan, on Windows and on the Steam Deck)
  --resolution_scale=<1-7>                   render scale (default: chosen from the desktop resolution)
  --pacman_playfield_offset=<pixels>         move the picture up (default 40; 0 = original position)
  --log_file=pacman.log --log_level=debug   write a log
  --license_mask=<n>                         licence bits (the default, 1, is the full game)
  --game_data_root=<dir or file>             use game files from another folder, or a package file
