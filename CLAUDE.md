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
    src\playfield.cpp       moves the picture up (playfield position, see below)
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
  cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=C:/PacManRecomp/tools/rexglue-sdk/win-amd64
  cmake --build --preset win-amd64-release        (codegen runs as part of the build; ~15 s)
Codegen alone (fast check of config changes): ..\tools\rexglue-sdk\win-amd64\bin\rexglue.exe codegen pacman_manifest.toml
Run: rexglue\out\build\win-amd64-release\pacman.exe (no arguments needed)
Use Release for playing and timing. The win-amd64-relwithdebinfo preset (links the SDK's *rd.dll builds) is for
debugging only: its loading screen takes ~40 s instead of a few, with one guest thread (F8000064) at 100% of a
core (measured 2026-10-20; the same source loads fast in Release). A failed configure (e.g. outside vcvars64:
"CMAKE_RC_COMPILER not set") leaves the build dir broken - re-run the configure command inside vcvars64.
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
- render resolution: the largest 16:9 area fitting the primary display's physical mode, divided by the game's
  1280x720 and rounded up (4K -> 3, 3440x1440 -> 2, 1920x1200 -> 2); logged as "render resolution: Nx".
  --resolution_scale / --draw_resolution_scale_x/_y or the config file override it (checked via
  GetFlagSource). These cvars live in rexgpu-xenos.dll and only exist once it is loaded, so OnPreSetup loads
  the plugin itself (ReXApp skips loading when config.graphics is set) and sets them with SetFlagByName;
  REXCVAR_DECLARE on them fails to link.
- mouse cursor: auto-hidden over the window (Window::CursorVisibility::kAutoHidden, set in OnCreateDialogs, the
  first hook after the window exists); shows while the mouse moves, hides after 1.5 s idle.
- keyboard controls (ApplyKeyboardControls): the SDK's MnkInputDriver (always created, off unless mnk_mode) is
  turned on; it is a synthetic device merged into guest user 0 with any real pad. Binds = comma lists of key
  names with exact modifier matching. Set: D-pad = Up,W / Down,S / Left,A / Right,D; A = Space,Return;
  B = Escape; Start = P; every other keybind_* cleared (SDK defaults put arrows on the right stick, Shift+arrows
  on the D-pad, etc.). Only binds still at their default are set. SDK source: src/input/mnk/mnk_input_driver.cpp.
- present_letterbox = true: any window/screen shape keeps the 16:9 image unstretched, with black bars.
- fullscreen = true (unless set elsewhere). F11 / Alt+Enter toggle it (PacmanApp::OnKeyDown; the SDK has no
  key for it). SDL's KeyEvent always has repeat_count 1 (auto-repeat is prev_state), and a mode switch makes
  SDL re-send the held key, so toggles within 500 ms are ignored. Other keys go to rex::ui::ProcessKeyEvent.

Playfield position (src\playfield.cpp, pacman_playfield_offset, default 40 = px at 720p, 0 = original):
the game centres its maze and pulsing neon frames 40 px below the screen middle, so the outer frame's bottom
is cut off (same in Xenia; display path checked: scaler source 0,0-1280x720, front buffer 1280x720). The
shift is applied in clip space, so the hidden strip gets drawn rather than clipped (a viewport shift would
leave an empty band). Two matrices, each hooked right after the game writes it:
- sprite matrix 0x821EDC30 (sub_8213B1C0, once per frame; uploaded to vertex constants c0-c3 by
  sub_820720B0 from the sprite draws sub_8213AB98 / sub_8213AD30): maze, characters, HUD, menus. c1 += ty*c3.
  The inverse at 0x821EDC70 is built from a stack copy, so it stays unshifted.
- startup perspective projection 0x821E5950 (sub_820607E0 -> sub_8207FCD8; FOV 45, 16:9, near 1, far 200;
  view at 0x821E5990, eye (0,0,-7)): drives the ghost-house clipping. Shifting only this one moves nothing
  but that clipping.
At an exaggerated 150 px one purple/yellow glow near the bottom stayed put (a third route); at 40 nothing
visible is off (owner play-tested 2026-10-20: gameplay, ghost house fine).
Shader dumps: --dump_shaders=<dir> (Xenia cvar) writes ucode disassembly; the sprite shaders are
2AED2C0D75EE82C9 / 892169D43B1134FB (c0-c3), the line shaders 6BAB79FEBEF71864 / BD88B0252E3E7DBB (c4-c7).

Exe icon: rexglue\pacman.ico, an ORIGINAL neon-maze design (no game artwork, safe to commit and ship),
linked via src\pacman.rc; SDL uses it for the window too. Regenerate with python Tools\make_icon.py (Pillow):
256 = PNG entry with the title, 64 with title, 48/32/16 = 32-bit bitmaps without it. (Namco's
game_icon.png is deliberately not used.)
Release plan (owner, 2026-10-20): eventually publish a ready-made zip (exe + DLLs, empty game\).

## Portable build
  powershell -ExecutionPolicy Bypass -File tools\package_portable.ps1 [-GameData C:\PacManRecomp\private] [-SkipBuild]
Builds the Release preset and assembles dist\PacManCE\ (git-ignored): pacman.exe, rexruntime.dll,
rexgpu-xenos.dll, the four VC++ runtime DLLs the binaries import (msvcp140, msvcp140_atomic_wait,
vcruntime140, vcruntime140_1, from VS's VC\Redist), README.txt (rexglue\README-portable.txt) and licenses\
(rexglue\licenses\ + SDK's SDL3 licence; ReXGlue's BSD licence requires its notice with binaries). -GameData
copies the game files into game\ (everything except default.exe). Re-running keeps game\ and userdata\.
Everything else the binaries import is part of Windows 10/11.

## Gameplay slowdown - SOLVED 2026-10-20 (root cause: host presentation coupled to the guest frame)
ReXApp always creates an achievement-toast ImGui dialog; any dialog registers ImGui as a UI drawer, which
forces the presenter to paint on the UI thread at the display refresh rate (the "120 fps"). The game's swap
then waited about one host refresh per frame for that painting's GPU work (shared D3D12 direct queue):
8.3 ms at 120 Hz, 16.7 ms at 60 Hz (locked the game to 30 fps). Heavy frames (ghost combos, the board's
"loop reset") missed their 60 Hz vblank -> lag storms while no thread was busy.
Fix (src\pacman_app.h): CreateAchievementNotificationDialog() returns nullptr (no achievement pop-ups;
achievements still recorded, F7 lists them) and host_present_from_non_ui_thread = true, so the presenter
presents from the guest output thread once per guest frame, as Xenia does. Result: 5.5 min run, 0.4 s behind
in total (was 7-18 s), no storm windows, command-buffer interrupt ~1-2 ms after VdSwap.
Mitigations re-measured afterwards (2026-10-20): pacman_fast_gpu_handshake no longer changes pacing and cost
~a core of busy-polling, so it is now OFF by default (gameplay: 5.5 min, 0.7 s behind, GPU thread 8-12%).
pacman_command_buffer_mb=8 kept: no buffer-full waits in gameplay, costs only 6 MB. History below.

## Steam Deck (Proton) - slow, investigated 2026-10-20; next step: native Linux build
Under Proton the game drops to 30/20 fps (vblank quantisation: 2 or 3 vblanks per frame) as dot grids fill
the maze; Deck overlay shows CPU and GPU both ~40-50%. Logs (owner's Deck, 1280x800, scale 1x):
- Bottleneck = GPU command processor: VdSwap -> command-buffer interrupt 10-12 ms per frame at 60 fps
  (PC: 1-2 ms), 18-23 ms with full dot grids. One thread translating Xenos -> D3D12, then vkd3d-proton
  D3D12 -> Vulkan, per draw; the overlay averages 8 threads so it doesn't show.
- Access faults (frame stats now count them): all write watches (none on GPU registers), 1 -> ~100 per frame
  with dots, ~80% on job-worker threads. Wine makes each costly, but they are not the main cost.
- Helped: --pacman_fast_gpu_handshake=true (clearly), --d3d12_tiled_shared_memory=false (a little).
  No effect: gamescope Allow Tearing / 60 Hz, TDP, frame limit.
- The Windows GPU plugin is D3D12-only. ReXGlue ships linux-amd64 SDK zips (same version; Vulkan backend),
  so a native Linux build skips Wine and vkd3d. Port needs: ShortSleep (waitable timer), frame_stats
  (VEH, toolhelp/GetThreadTimes), EnumDisplaySettingsW, pacman.rc. Needs WSL (not installed yet) or another
  Linux build environment.
- 2026-10-21 findings: the game issues one draw call per sprite: sub_8213AB98 / sub_8213AD30 set texture,
  shaders, vertex constants c0-c3 and per-sprite constants (device+0x740..0x75C), then draw a 4-vertex quad
  via sub_820CB840(dev, 6, 0, 4). Frame stats now count these: 200-450 per frame in menu/attract (gameplay
  with dot grids not yet measured). On PC the "GPU Commands" thread handles that in ~1 ms/frame; the Deck's
  10-23 ms fits the same per-draw work ~10x slower through Wine + vkd3d-proton.
- Middle option before a Linux port: the SDK builds the Vulkan backend on Windows too (REXGLUE_USE_VULKAN, OFF
  in the prebuilt win-amd64 SDK; plugin_main.cpp picks the backend from GpuCreateInfo::backend). A Windows
  build with Vulkan skips vkd3d-proton under Proton (winevulkan passes through to RADV).
- Vulkan build (2026-10-21, awaiting a Deck test): SDK source at the prebuilt's commit bd833a2a in
  tools\rexglue-sdk-src (ignored; submodules shallow, moltenvk skipped). Git on Windows checks libmspack's
  symlinks (cabextract/mspack/*) out as text files - replaced with copies of their targets. Configure inside
  vcvars64: cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DCMAKE_INSTALL_PREFIX=C:/PacManRecomp/tools/rexglue-sdk-vk/win-amd64
  then cmake --build out/build/win-amd64 --config Release --target install --parallel (Release only, ~5 min).
  App: --pacman_gpu_backend=any|d3d12|vulkan (main.cpp); OnPreSetup passes it to LoadGpuPlugin, "any" becomes
  vulkan under Wine (ntdll exports wine_get_version), falling back to the default if the plugin lacks it.
  package_portable.ps1 -Vulkan builds out\build\win-amd64-release-vk -> dist\PacManCE-Vulkan.
  On Windows the Vulkan backend renders correctly (screenshots matched D3D12 at 1x and 3x), 60 fps, similar
  CPU; it logs "Vulkan draw resolution scaling is experimental". Imports unchanged (vulkan-1.dll loaded at
  runtime). Before a public release: add licences for glslang, SPIRV-Tools, VMA, Vulkan headers.
- Deck result with Vulkan: largely unchanged. Profile (--pacman_profile, same session): GPU Commands ~45% in
  memory::Protect (PhysicalHeap::EnableAccessCallbacks from SharedMemory::MakeRangeValid) + RequestRanges,
  ~34% sleeping in ExecutePacketType3_WAIT_REG_MEM; main thread ~50% waiting for the GPU, ~15% in access-fault
  handling (MMIOHandler::ExceptionCallback, Protect). Cause: the game writes per-draw vertex data into a ring
  just ahead of the command processor, so the same pages ping-pong upload -> protect -> fault -> upload; on
  Wine every protect and fault is slow. Grows with sprites on screen.
- Fix: SDK patch patches\rexglue-sdk\0001-* (branch "pacman" in tools\rexglue-sdk-src, on bd833a2a):
  shared_memory_streaming_blocks - a 64-page block invalidated 8+ times in 1 s is left unprotected and never
  valid (re-uploaded on every request); not for blocks with texture watches, GPU-written data or global
  watches (scaled resolve); WatchMemoryRange un-streams and re-protects; marks reset every 10 s. PC, Vulkan
  1x attract: faults 14-25 -> 4 per frame, picture correct. App (ApplyWineDefaults in pacman_app.h): under
  Wine turns it on plus pacman_fast_gpu_handshake. Awaiting a Deck test.
- Re-applying the patch to a fresh SDK clone: git -C tools\rexglue-sdk-src am ..\..\patches\rexglue-sdk\*.patch
  (plus the libmspack symlink fix). The win-amd64-release-vk build dir only re-copies SDK DLLs when pacman.exe
  relinks - after an SDK-only change copy rexgpu-xenos.dll / rexruntime.dll in by hand (or touch a source).

## Known issues
- (Resolved, see above) Game-logic slowdown in places (owner, 2026-10-20 portable build): presented frame rate stays constant,
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
  - Mid-frame wait decoded: sub_8206FA78 (start a command segment) -> sub_8206EED0 (command-buffer ring
    allocator, device+14752..14756) -> sub_8206EAD8 waits for the GPU's read position (status block +4) to
    pass the space being reused = the 2 MB command buffer was full. sub_8206F728 is a fence wait (completed
    fence = status block +0, CPU counter device+10780; throttling fence device+14788 set in sub_82067A18).
  - Fix: pacman_command_buffer_mb (default 8): hook in D3D setup sub_82070038 on D3DRING_BUFFER_PARAMETERS
    (+12 secondary size, default 2 MB; +20 segment count, default 32) -> 8 MB / 128 x 64 KB segments. Only the
    segment size is stored (device+0x39A8), so more segments is safe. Storm cost 17 -> 10 -> 5.6 s; buffer-full
    waits now 0.
  - Owner: storms happen at the board's "loop reset", when it refills with power pellets (seen twice).
    Remaining storms show 0 buffer-full waits, ~1 fence wait per frame, nothing saturated.
  - WAIT_REG_MEM builders: sub_82067570 (2 handshake waits, wait 0x100), sub_82067838 (flip wait on reg 0x1973
    bit 0, wait 0x100, conditional on r24 & 4), sub_8206F3A8 / 8207A500 / 8207BA40 / 8207D7C0 (reg 0xA31,
    wait 8 - fine). The flip wait got the same 0xFF hook (0x8206792C, r15) - awaiting a gameplay test.
- FIXED 2026-10-20: heap-corruption crash (0xC0000374) on every exit not via the window's close button (e.g.
  quitting from the game's menu): ReXApp::OnDestroy destroys window_ before runtime_, and ~Runtime ->
  InputSystem::Shutdown -> ~MnkInputDriver -> DetachFromWindow() touched the freed window (it only detaches
  early on the window's OnClosing). Fix: PacmanApp::OnShutdown (called first in OnDestroy) shuts the input
  system down while the window exists; InputSystem::Shutdown is idempotent. A ReXGlue bug - worth reporting.
  Windows writes the dumps silently to %LOCALAPPDATA%\CrashDumps (keeps the newest 10) - check them after
  changes. Minidumps can be read without a debugger: exception stream + scanning the thread's stack for module
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
- Built-in sampler (src\profiler.cpp, --pacman_profile=true, off by default; works under Proton): every 2 ms
  suspends Main XThread, GPU Commands and the UI thread, unwinds with RtlVirtualUnwind over .pdata copied
  beforehand (no locks while a thread is suspended), logs every 10 s per thread: leaf-module shares (ntdll =
  waiting in a syscall) and the top spots as "leaf module | first own frame <- second own frame"
  (module+0xRVA in pacman.exe / rexruntime.dll / rexgpu-xenos.dll). Symbolize offline with llvm-symbolizer
  --obj=<binary> 0xRVA+ImageBase (needs the PDBs of exactly those binaries: the Vulkan build is made with
  -gline-tables-only -gcodeview and -Wl,-debug for this). Windows baseline in attract: main thread 87% in
  ntdll (D3D GPU wait sleep), GPU Commands 75% in ntdll (waiting for work).
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
