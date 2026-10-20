Pac-Man Championship Edition - native PC build
==============================================

Setup
  Put your own copy of the game's files in the game\ folder, so that it contains:
    game\default.xex
    game\media\...
    game\data\...
  Then run pacman.exe. No installation is needed; the whole folder can be moved or copied.

Files
  pacman.exe, *.dll   the game and its runtime (Visual C++ runtime included)
  game\               your game files (not included)
  userdata\           created on first run: saves and the shader cache. Delete it to start fresh.
  pacman.toml         optional ReXGlue settings file, read at startup if present (not created automatically)
  licenses\           third-party notices

Requirements
  64-bit Windows 10 or 11 with a Direct3D 12 GPU.

Options (on the command line, e.g. pacman.exe --fullscreen)
  --log_file=pacman.log --log_level=debug   write a log
  --license_mask=<n>                         licence bits (the default, 1, is the full game)
  --game_data_root=<dir>                     use game files from another folder
