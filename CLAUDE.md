# Pac-Man Championship Edition (Xbox 360 XBLA) - static recompilation

Hobby project: native PC port of Pac-Man CE, built on the ReXGlue SDK (github.com/rexglue/rexglue-sdk,
BSD-3, Xenia-based: kernel/XAM, D3D12/Vulkan GPU, XMA audio, VFS, input). Windows, VS 2026 (v18), Clang 22.
Game data comes from the owner's own console dump and is never committed or shared.
Goal beyond "it runs": Xenia plays CE well but is GPU/CPU-heavy; this port should run light.
Status: plays flawlessly (owner-confirmed): full video/audio, full version, ~7% CPU in gameplay, quiet fans.

## Layout (C:\PacManRecomp)
- private\                  game data: default.xex, media\, data\ (+ default.exe, the PE basefile from
                            xextool -b; ImageBase 0x82000000). Ignored by git; never modify or commit.
- rexglue\                  the ReXGlue project (created by rexglue init)
    pacman_manifest.toml    project + entrypoint; includes the two files below
    pacman_config.toml      codegen config: setjmp/longjmp, function boundaries, mid-asm hooks
    extra_functions.toml    74 function starts ReXGlue's analysis misses (vtable-only methods, no .pdata)
    src\pacman_app.h        ReXApp subclass: run defaults (see below)
    src\spin_hooks.cpp      spin-wait backoff hook functions
    generated\rexglue.cmake SDK integration (tracked); generated\default\ = codegen output (game code, ignored)
    out\                    build output (ignored)
- tools\rexglue-sdk\win-amd64\  prebuilt SDK nightly 0.10.0.24 (downloaded, ignored). bin\rexglue.exe = CLI.
- tools\ppcinspect.py       disassembler helper (capstone): modes fn / dis / words / bytes, on private\default.exe
- Legacy XenonRecomp work: see the last section.

## Build environment
Scripted builds run inside vcvars64 (not Developer PowerShell - it defaults to x86):
- vcvars64.bat needs "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer" (vswhere) on PATH. It prints
  VsDevCmd errors but the x64 env still works, so chain commands with "&", not "&&".
- In a one-line cmd /c chain %ERRORLEVEL% is expanded at parse time - don't trust it.
- The ReXGlue presets use clang/clang++ (GNU driver): put VS's LLVM bin on PATH,
  C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin
- From Git Bash, prefix MSYS_NO_PATHCONV=1 to anything taking /flags, or they get mangled into paths.

## Build and run
From rexglue\ (configure once):
  cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=C:/PacManRecomp/tools/rexglue-sdk/win-amd64
  cmake --build --preset win-amd64-relwithdebinfo        (codegen runs as part of the build; ~15 s)
Codegen alone (fast check of config changes): ..\tools\rexglue-sdk\win-amd64\bin\rexglue.exe codegen pacman_manifest.toml
Run: rexglue\out\build\win-amd64-relwithdebinfo\pacman.exe (no arguments needed)
  Useful options: --log_file=<path> --log_level=debug|trace, --license_mask=<n>, --game_data_root=<dir>
Defaults set in src\pacman_app.h (command-line values still win, except that an explicit empty/0 value
can't be told apart from "not set"):
- game data, first match: game\ next to the exe (portable layout), the exe's folder, then the nearest private\
  searching up from the exe (repo layout)
- user data + shader cache: userdata\ next to the exe; falls back to the SDK default Documents\pacman when
  that can't be created (e.g. Program Files)
- GPU: gpu_plugin = "xenos" (the SDK default is no GPU emulation; CMakeLists passes GPU_PLUGINS xenos so
  the DLL is staged next to the exe)
- license_mask = 1 (XamContentGetLicenseMask; bit 0 = purchased) so it runs as the full game, not the XBLA
  trial. The startup log prints the effective value.

## Portable build
  powershell -ExecutionPolicy Bypass -File tools\package_portable.ps1 [-GameData C:\PacManRecomp\private] [-SkipBuild]
Builds the Release preset and assembles dist\PacManCE\ (git-ignored): pacman.exe, rexruntime.dll,
rexgpu-xenos.dll, the four VC++ runtime DLLs the binaries import (msvcp140, msvcp140_atomic_wait,
vcruntime140, vcruntime140_1, from VS's VC\Redist), README.txt (rexglue\README-portable.txt) and licenses\
(rexglue\licenses\ + SDK's SDL3 licence; ReXGlue's BSD licence requires its notice with binaries). -GameData
copies the game files into game\ (everything except default.exe). Re-running keeps game\ and userdata\.
Everything else the binaries import is part of Windows 10/11.

## Known issues
- Game-logic slowdown in places (owner, 2026-10-20 portable build): presented frame rate stays constant,
  music keeps real-time pace, game falls ~18 s behind over a 5 min run (~6%). No visible CPU/GPU change.
  The game steps once per guest frame, so any frame that misses its 60 Hz slot slows the game.
  Worst when eating ghosts in power mode, especially with the score multiplier maxed (the heaviest frames);
  happens in different spots each run.
  Shader compilation is unlikely: the cache loads 12 shaders / 13 pipelines at startup and the log shows no
  errors. Main suspect: the GPU-wait hook - heavy frames fill the ring buffer, so the main thread waits on the
  GPU several times a frame and each wait can oversleep by up to 250 us; the worker hooks similarly.
  Tools (src\spin_hooks.cpp, src\frame_stats.cpp):
    --pacman_gpu_wait_backoff=false / --pacman_worker_backoff=false   turn a hook kind off (original busy-wait)
    --pacman_frame_stats (default on): every 10 s logs frames, fps, frames over 20 ms, worst frame and
      "game behind by" (elapsed - frames/60) - the drift the owner measured. FrameHook is on the game's only
      VdSwap call (0x82067C64).
  Measured 2026-10-20 in the menu (steady 10 s windows, after the startup window):
    defaults: 15-46 frames over 20 ms, 84-183 ms lost; both hooks off: 51-121 over 20 ms, 267-433 ms lost
    (worse - the hooks are NOT the cause); GPU-wait hook off: 9, 84 ms; + timeBeginPeriod(1): 11-13,
    100-116 ms (no change - timer resolution is not the cause; reverted).
  Owner played those test instances: the defaults run and the timeBeginPeriod(1) run both felt much better
  in gameplay, at ~4% CPU. Gameplay is far more affected than the menu, so measure there next.
  Slow frames are ~33 ms = one missed vblank, so the game is losing 1-2% of frames even in the menu, more in
  heavy scenes.
  Findings from gameplay logs (frame stats now also log per-thread CPU, main-thread GPU-wait share, vblank
  interrupt timing and command-buffer interrupt delay after VdSwap):
  - Lag storms (tens of seconds at ~35-45 fps, 2-4 s lost per 10 s) start in heavy scenes (ghost combos) and
    nothing is busy: main thread ~5-10% CPU and 90-98% waiting on the GPU, "GPU Commands" 5-12%.
  - Vblank interrupts are perfect (600/10 s, max gap 18 ms) - delivery is not the problem.
  - Exactly one command-buffer interrupt (source 1) per frame, ~13 ms after VdSwap normally, ~8.5-11 ms in
    storms: the main thread spends ~25 ms between the flip interrupt and its next VdSwap, waiting on the GPU
    mid-frame. Likely: heavy frames need more command space than is free, D3D waits for the command
    processor, which is parked on the previous frame's swap handshake until the flip.
  - Handshake fix (pacman_fast_gpu_handshake: wait field 0xFF on D3D's two per-frame WAIT_REG_MEM packets in
    sub_82067570, so the command processor polls instead of Sleep(1)): storms remain but are much milder;
    without it lag was constant. On by default; ~1-2% CPU.
  Next: find what sub_8206F728's mid-frame wait is waiting for (ring/command-buffer space vs a fence) and
  whether more command space or letting D3D run a frame further ahead removes the stall.
- Intermittent heap-corruption crash (0xC0000374) on exit, inside the SDK: ReXApp::OnDestroy -> ~Runtime ->
  Runtime::Shutdown -> InputSystem::Shutdown -> input device destructors. Seen in two of the owner's sessions
  (dumps in %LOCALAPPDATA%\CrashDumps); clean CloseMainWindow() exits worked. Gameplay unaffected.
  Minidumps can be read without a debugger: exception stream + scanning the thread's stack for module
  addresses, then llvm-symbolizer (pacman.pdb) / nearest export (SDK DLLs ship no PDBs).

## Codegen config notes
- After codegen, check for "has no blocks - generating stub" and analysis errors (e.g. UnresolvedCall).
- Function boundaries: [functions] keyed by hex address, { size = } or {} (size left to analysis).
- extra_functions.toml came from diffing ReXGlue's function list against XenonRecomp's. Excluded on purpose:
  20 SEH records XenonRecomp mistook for functions (word 0x821BAB5C = __C_specific_handler thunk + a scope
  table pointer, placed just before a function, e.g. 0x82063890 before 0x82063898).
- 0x8208A000 (size 0x28): leaf function reached only via a this-adjusting thunk (b from 0x8208972C).
- ReXGlue detects the save/restore helpers itself and found all 77 jump tables; it implements every
  instruction the game uses. Codegen emits the extern declarations for mid-asm hook functions.

## Efficiency (spin-wait hooks)
The CPU cost was four guest threads each spinning at 100% of a core (on the Xbox each had a hardware
thread to itself):
- job workers 1-3: sub_8213ED08 / sub_8213EDE0 / sub_8213EEB8 poll a control word (0x821EDD30, +8, +16)
  for state 1 with no delay. Hooked at the "no job" bne: 0x8213ED54 / 0x8213EE2C / 0x8213EF04 (r11 = state).
- main thread in D3D's GPU wait poll sub_82065018 (called in a loop by sub_8206F728). Hooked at 0x8206506C.
src\spin_hooks.cpp spins briefly with _mm_pause, then sleeps in 250 us steps on a high-resolution waitable
timer. Menu: 26% -> 2% of all cores; gameplay ~35% -> ~7%, with no change in feel.
120 fps is host presentation at the display's refresh rate; the guest vblank is 60 Hz
(video_mode_refresh_rate), so game speed is correct.

## Profiling
- Per-thread CPU: GetThreadTimes + GetThreadDescription over a sample window (guest threads are named
  "XThreadNNNN (handle)", the main one "Main XThread"). Start the exe, sample, then CloseMainWindow().
- Stack sampling: an external sampler that suspends guest threads and walks stacks with DbgHelp, using
  pacman.pdb for sub_XXXXXXXX names. It slows game logic badly while it runs - warn the owner if they're
  watching. (These tools were scratch code, not in the repo.)
- --perf_log_csv wrote nothing in testing.
- Expected noise in logs: NtCreateFile failures for game:\media\en-us\* (the game tries the language folder,
  then media\); a missing SDL gamecontrollerdb.txt.

## Game facts (stable, from analysis)
- Title ID 58410877. Code 0x82060000-0x821BB4DC; image 0x82000000-0x82210000. Same MSVC runtime as Sonic
  Unleashed.
- setjmp 0x8215BDB0, longjmp 0x8215BC90 (MSVC CRT). setjmp's SEH hook pointer 0x821F6330 is never set, so
  the plain path is always taken.
- sub_821B7F8C is not longjmp: it restores a CONTEXT record and jumps to r3 (an EH continuation helper).
- CRT: _mtinit sub_820B8F58, _lock sub_820BC780, _mtinitlocknum sub_820BC640, CreateThread wrapper sub_820CA7A0,
  XapiThreadStartup 0x820CACB0.
- D3D: interrupt callback sub_8206ED50 (context 0x40001600); GPU registers at 0x7FC80000 (CP_RB_WPTR
  0x7FC80714). D3D inlines virtual->physical: (va & 0x1FFFFFFF) + 0x1000 if va >= 0xE0000000.
- The game reads PCR fields r13+0x100 (thread), +0x10C (cpu), +0x150 (dpc active).

## Legacy: XenonRecomp + hand-written host (superseded 2026-10-20)
Before ReXGlue the port used XenonRecomp plus our own host runtime; it reached file I/O with memory, TLS,
threads/sync and a null GPU implemented. Kept in the repo for reference; not built by default.
- config\pacman.toml, config\switch_tables.toml   XenonRecomp config (77 jump tables, one added by hand)
- CMakeLists.txt (root), host\                     PacManHost.exe; host\import_stubs.cpp is generated by
                                                   tools\gen_import_stubs.py from ppc\ppc_func_mapping.cpp
- ppc\ (ignored)                                   XenonRecomp output. NEVER read wholesale - search it.
- tools\xenonRecomp\ (ignored, separate clone)     XenonRecomp on local branch pacman-instructions;
  patches\xenonRecomp\                             our 3 patches on upstream ddd128b (PR #181 cherry-pick + 14
                                                   instructions). Restore: git -C tools\xenonRecomp am patches\xenonRecomp\*.patch
- docs\imports.md                                  the 164 imports the game uses, by subsystem
Lessons worth keeping:
- XenonRecomp emits __builtin_debugtrap() for some recognised-but-unimplemented instructions without
  logging them; grep generated code for traps, not just the log.
- XenonRecomp emits mftb as __rdtsc(), so guest timing needs the host TSC frequency, not 50 MHz.
- Recompile: tools\xenonRecomp\build\XenonRecomp\XenonRecomp.exe config\pacman.toml tools\xenonRecomp\XenonUtils\ppc_context.h
