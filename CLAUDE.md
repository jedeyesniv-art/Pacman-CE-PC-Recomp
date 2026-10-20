# Pac-Man Championship Edition (Xbox 360 XBLA) - static recompilation

Hobby project: native PC port of Pac-Man CE using XenonRecomp. Windows, VS 2026 (v18), Clang 22.
Game data comes from the owner's own console dump and is never committed or shared.

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
   0xA0000000/0xC0000000/0xE0000000 - not aliased to each other). The CRT heap now initialises.
   Current blocker: KeTlsSetValue stub returns 0 (= failure), so CRT _mtinit (sub_820B8F58) fails before
   _mtinitlocks (sub_820BC528); the error path then recurses _lock (sub_820BC780) <-> _mtinitlocknum
   (sub_820BC640) on lock 10 until the guest stack guard trips.
   Next: KeTlsAlloc/Free/GetValue/SetValue, then real critical sections (Rtl*CriticalSection are stubs).
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
