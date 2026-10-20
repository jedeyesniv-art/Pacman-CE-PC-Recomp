# Pac-Man Championship Edition (Xbox 360 XBLA) - static recompilation

Hobby project: native PC port of Pac-Man CE. Windows, VS 2026 (v18), Clang 22.
Game data comes from the owner's own console dump and is never committed or shared.

## ReXGlue port (branch rexglue, 2026-10-20) - the current approach
The port moved from XenonRecomp + our own host (still on master) to the ReXGlue SDK
(github.com/rexglue/rexglue-sdk, BSD-3, Xenia-based: kernel/XAM, D3D12/Vulkan GPU, XMA audio, VFS, input).
Owner's goal beyond "it runs": Xenia plays CE well but is very GPU-heavy; this port should run light.
- tools\rexglue-sdk\win-amd64\  prebuilt SDK nightly 0.10.0.24 (downloaded, not tracked). bin\rexglue.exe = CLI.
- rexglue\                      the ReXGlue project (rexglue init): pacman_manifest.toml includes
    pacman_config.toml          setjmp/longjmp, our function boundaries, the 0x8208A000 leaf function
    extra_functions.toml        74 function starts XenonRecomp found but ReXGlue's analysis missed (vtable-only
                                methods, no .pdata), e.g. 0x82083BA8. NOT the 20 SEH records (word 0x821BAB5C
                                = __C_specific_handler + scope table, placed before a function) XenonRecomp
                                mistook for functions - those give "has no blocks - generating stub".
    src\pacman_app.h            ReXApp subclass (hooks); generated\default\ = codegen output (game code, ignored)
- ReXGlue auto-detects save/restore helpers and found all 77 jump tables itself; it implements every
  instruction we had to patch into XenonRecomp.
Build (vcvars64 env as below, plus VS's LLVM bin on PATH for clang/clang++), from rexglue\:
  cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=C:/PacManRecomp/tools/rexglue-sdk/win-amd64
  cmake --build --preset win-amd64-relwithdebinfo      (codegen runs as part of the build; ~15 s total)
Run (from rexglue\out\build\win-amd64-relwithdebinfo):
  pacman.exe --game_data_root=C:/PacManRecomp/private --gpu_plugin=xenos [--log_file=... --log_level=debug]
  Without --gpu_plugin=xenos there is no GPU emulation. CMakeLists passes GPU_PLUGINS xenos so the DLL is staged.
Status 2026-10-20: builds and runs, confirmed by the owner: full video and audio, plays perfectly at 120 fps
(D3D12, RTX 5070 Ti) with ~9% GPU and ~30% CPU; fans still spin up noticeably. Next: efficiency pass
(CPU use, frame rate vs the game's native 60 Hz, spin-waits) - measure with --perf_log_csv.
The game probes game:\media\en-us\ first and falls back to media\ - those NtCreateFile failures are expected.

The sections below describe the XenonRecomp-based work (master), kept for reference.

## Layout (C:\PacManRecomp)
- private\default.xex      original executable (do not modify)
- private\default.exe      decrypted/decompressed PE basefile (from xextool -b); ImageBase 0x82000000
- config\pacman.toml       main XenonRecomp config
- config\switch_tables.toml  jump tables from XenonAnalyse (+ manual additions)
- ppc\                     generated C++ (~30 files). NEVER read wholesale - search it.
- tools\xenonRecomp\       XenonRecomp source + build\ (separate clone, not tracked by this repo)
- patches\xenonRecomp\     our XenonRecomp changes as format-patch files, based on upstream ddd128b.
                           Restore into a fresh clone: git -C tools\xenonRecomp am patches\xenonRecomp\*.patch
                           After new commits on pacman-instructions, delete the old *.patch files and regenerate:
                           git -C tools\xenonRecomp format-patch ddd128b..pacman-instructions -o C:\PacManRecomp\patches\xenonRecomp
- docs\imports.md          the 164 Xbox imports the generated code needs from a host runtime, by subsystem
- CMakeLists.txt, host\    host program PacManHost.exe (links all of ppc\ + XenonUtils for XEX loading)
  host\import_stubs.cpp    GENERATED weak logging stubs for every import (tools\gen_import_stubs.py; re-run
                           after recompiles that change imports). Implement an import by writing a normal
                           PPC_FUNC(__imp__<Name>) anywhere in host\ - it overrides the weak stub.
- tools\ppcinspect.py      disassembler helper (capstone): modes fn / dis / words / bytes
- tools\find_save_restore.py

## Build environment
Use "x64 Native Tools Command Prompt for VS" (NOT Developer PowerShell - it defaults to x86).
clang-cl is not on PATH there; pass the full path:
  C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe
After a failed CMake configure, delete build\ before retrying.
Scripted builds: vcvars64.bat needs "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer" (vswhere) on PATH;
it then prints VsDevCmd errors but the x64 env still works, so chain with "&" not "&&". Existing build\ is
Ninja + clang-cl Release: cmake --build tools\xenonRecomp\build --target XenonRecomp

## Recompile command (from C:\PacManRecomp)
tools\xenonRecomp\build\XenonRecomp\XenonRecomp.exe config\pacman.toml tools\xenonRecomp\XenonUtils\ppc_context.h > recomp_log.txt 2>&1

## Building the generated code (no host project yet)
Compile each ppc\*.cpp from inside ppc\ (16 in parallel takes ~6 s total):
  clang-cl /std:c++20 /O2 /EHsc -march=sandybridge -fno-strict-aliasing -I. -I<xenonRecomp>\thirdparty\simde -c <file>
From Git Bash, prefix MSYS_NO_PATHCONV=1 or the /O2-style flags get mangled into paths. llvm-lib archives the
objects. A trial link (stub main walking PPCFuncMappings, inside the vcvars64 env) fails only on the imports
listed in docs\imports.md. Note: in a one-line cmd /c chain %ERRORLEVEL% is expanded at parse time - don't trust it.

## Host program
Configure once (inside the vcvars64 env, see above), then build:
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=<clang-cl> -DCMAKE_CXX_COMPILER=<clang-cl>
  cmake --build build
Run from C:\PacManRecomp: build\PacManHost.exe [path\to\default.xex]   -> log in host_log.txt (also stderr)
Exit codes: 3 fatal stub (KeBugCheck etc.), 4 host guest-memory region full, 5 indirect call to unmapped
guest address, 6 host exception (crash report names the sub_XXXXXXXX it happened in, the guest fault address,
guest regs and the last stub called).
Guest memory: one 4 GB block (g_base + guest addr), page 0 no-access. Host-owned guest structures (stacks, PCR,
data-import blocks) come from GuestAlloc in 0x7C000000-0x7FFFFFFF - the game's allocators must avoid it.
Guest stacks (GuestAllocStack) have a 64 KB no-access guard below them; the crash report flags hits.
Crash reports include a host backtrace (sub_XXXXXXXX names) - recursion shows up as a repeating pair.
Main thread: r1 = stack top - 0x100, r13 = PCR (Xenia X_KPCR layout: tls 0x0, self 0x30, stack 0x70/0x74,
current thread 0x100; thread object and TLS slots are zeroed placeholders).
Loader: XenonUtils leaves type-0 import records byte-swapped; host resolves each to its function thunk
(matched by name) or, for the 10 data imports, a zeroed 4 KB block.

## Known facts
- Code range ~0x82070000-0x821C0000 (~1.5 MB). Same compiler runtime as Sonic Unleashed:
  save/restore helper spacing matches SWA exactly.
- Save/restore helpers: savegprlr_14 0x820B8760, restgprlr_14 0x820B87B0, savefpr_14 0x820B9620,
  restfpr_14 0x820B966C, savevmx_14 0x8215B050, restvmx_14 0x8215B2E8, savevmx_64 0x8215B0E4, restvmx_64 0x8215B37C
- setjmp 0x8215BDB0, longjmp 0x8215BC90 (MSVC CRT; set in pacman.toml, 2026-10-20). 5 setjmp and 8 longjmp
  call sites (one a tail call) all become host setjmp/longjmp. setjmp's SEH hook pointer 0x821F6330 is 0 in
  the image and never written, so longjmp's RtlUnwind path is never taken.
- sub_821B7F8C is NOT longjmp: restores from a CONTEXT record (GPRs @136, FPSCR @288, FPRs @408) and jumps
  to r3 - likely an exception-handling continuation helper.

## Status / open issues (fourth recompile run, 2026-10-20)
Recompile completes cleanly (100%) with an empty log, and the generated code contains no __builtin_debugtrap().
XenonRecomp also emits silent debugtraps for recognized-but-stubbed instructions that it does NOT log, so after
every recompile also check: grep -n -B3 __builtin_debugtrap ppc/*.cpp
tools\xenonRecomp is on local branch pacman-instructions (on top of upstream ddd128b; backed up in patches\xenonRecomp):
  a8e7b38 cherry-picked upstream PR #181 (lhbrx, vandc, vsel128, mulhd, eqv, bso/bns, vpkswss)
  da486ee our additions: mulhdu, vaddsws, vavgsw, vminsw, vpkshss(128), vctuxs/vcfpuxws128, stvebx
          (new helpers simde_mm_adds_epi32 / simde_mm_avg_epi32 / simde_mm_vctuxs in ppc_context.h,
          tested vs scalar reference; vpkshss/vctuxs adapted from upstream PR #60, which is stale/unmergeable)
  fdc35ee our additions: vcmpbfp(128) (+ record form), blrl (indirect call via lr, like bctrl),
          vupkd3d128 float16_2/float16_4 (simde_mm_cvtph_ps), vpkd3d128 float16_2
          (simde_mm_vcmpbfp helper and half conversion tested vs scalar reference; D3D lane layout from Xenia)
Remember vector registers are stored fully reversed (see XenonRecomp README) - pack/select ops need care.
1. Half-float caveat: vupkd3d128 unpack uses IEEE halves (like Xenia), but upstream's float16 pack writes 0x7FFF
   for overflow/NaN. If Xenos treats exponent 31 as a normal number, unpacking such values is wrong.
2. blrl site sub_821B7F30 looks like MSVC's _CallSettingFrame (C++ EH funclet call; 0x8215C880 probably
   _NLG_Notify) - unverified. If so it only runs during exception unwinding.
3. Host runtime started (2026-10-20). Implemented: memory imports (host\kernel\memory_imports.cpp on
   GuestHeap: virtual 4K 0x00010000-0x3FFFFFFF, virtual 64K 0x40000000-0x7BFFFFFF, physical windows
   0xA0000000/0xC0000000/0xE0000000 - not aliased to each other); dynamic TLS (host\kernel\tls_imports.cpp,
   64 slots from the XEX TLS header, values per host thread; the header has no static TLS data and no guest
   code reads r13+0); null GPU (host\gpu\null_gpu.cpp: a host thread executes the ring buffer from
   CP_RB_WPTR, running only PM4 packets that write registers/memory - MEM_WRITE, EVENT_WRITE_SHD, COND_WRITE,
   REG_RMW, INDIRECT_BUFFER, INTERRUPT - and fires the interrupt callback for 60 Hz vblank; registers live in
   guest memory at 0x7FC80000). D3D init now completes.
   Threads & sync (host\kernel\thread_imports.cpp, sync_imports.cpp, dispatcher.cpp, handles.cpp): one host
   thread per guest thread; ExTerminateThread throws GuestThreadExit to unwind to the thread entry. Dispatcher
   objects (KEVENT/KSEMAPHORE/KTHREAD) keep their signal state in guest memory (+4) under one host lock;
   waiters re-check every 10 ms in case guest code changes a state directly. Handles 0xF8000004+, pseudo
   handle 0xFFFFFFFE = current thread. Critical sections are host locks keyed by guest address; spinlocks
   are real (atomic on the guest word); IRQL not modelled. KeQueryPerformanceFrequency reports the host TSC
   rate, because XenonRecomp emits mftb as __rdtsc().
   The game now creates its two D3D worker threads, finishes video setup, registers an XAudio client,
   starts networking, queries sign-in state, then opens files.
   Current blocker (files): NtCreateFile etc. are stubs. First files: game:\media\font_ranking.xpr (data is
   in private\media, private\data). With the stub, NtQueryInformationFile gives a garbage size, a 1 GB
   allocation fails, and the game calls XamLoaderLaunchTitle (exit to dashboard).
   Next: file system - map game:\ to private\, NtCreateFile/NtReadFile/NtQueryInformationFile/
   NtQueryFullAttributesFile/NtClose for files, RtlInitAnsiString and friends.
   Physical addresses: phys = (va & 0x1FFFFFFF) + 0x1000 if va >= 0xE0000000 (D3D inlines this in
   sub_82070038). Each physical window owns a slice of physical RAM so GuestPhysicalToVirtual is exact:
   64K pages phys 0-0x0FFFFFFF @0xA0000000, 16M pages 0x10000000-0x13FFFFFF @0xD0000000, 4K pages
   0x14000000-0x1FD00FFF @0xF3FFF000. Guest contexts (stack, PCR, thread object): host\guest_thread.cpp.
   Guest r13 fields the game reads: 0x100 (thread), 0x10C (cpu), 0x150 (dpc active) - matches Xenia's X_KPCR.
   Device registers live at 0x7F000000+ (GPU at 0x7FC80000); the host region stops at 0x7F000000.
   Hang reporter: if nothing is logged for 10 s, the host logs the guest main thread's backtrace and regs.
   Heavy ones later: Vd (GPU, 20) and XAudio/XMA (7). UnleashedRecomp's kernel layer is a reference (check
   its licence before copying). Known gaps: static TLS (XEX has a TLS header), data imports are zero-filled
   (KeTimeStampBundle etc. need real contents), PCR/thread object mostly zero.

Resolved (verified 2026-10-20):
- Full generated-code build: all 27 TUs compile cleanly; trial link leaves only the 164 guest imports.
- Silent debugtraps (8 sites: vcmpbfp 2, vupkd3d128 float16 4, vpkd3d128 float16_2 1, blrl 1) - fdc35ee.
- Unsupported instructions (110 sites: vaddsws 46, lhbrx 18, vpkshss 14, vminsw 14, vavgsw 8, vandc 5,
  vsel128 2, vctuxs 1, stvebx 1, mulhdu 1) - implemented on the pacman-instructions branch, see above.
- Function boundaries: all 5 switch-containing functions defined in pacman.toml; no boundary errors.
- Missed jump table at 820ACD1C (bctr): manual switch_tables.toml entry works - base 0x820ACCF8
  (the lis r12 after the bounds check, XenonAnalyse's convention), r30, 9 cases.
