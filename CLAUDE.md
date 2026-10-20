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

## Known facts
- Code range ~0x82070000-0x821C0000 (~1.5 MB). Same compiler runtime as Sonic Unleashed:
  save/restore helper spacing matches SWA exactly.
- Save/restore helpers: savegprlr_14 0x820B8760, restgprlr_14 0x820B87B0, savefpr_14 0x820B9620,
  restfpr_14 0x820B966C, savevmx_14 0x8215B050, restvmx_14 0x8215B2E8, savevmx_64 0x8215B0E4, restvmx_64 0x8215B37C
- setjmp/longjmp: not located yet (omitted from config).

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
3. Not yet done: building/linking the full generated ppc\ code (only ppc_recomp.2/8/15/24.cpp were test-compiled
   with clang-cl -c); nothing has run in-game; new instructions untested at runtime.

Resolved (verified 2026-10-20):
- Silent debugtraps (8 sites: vcmpbfp 2, vupkd3d128 float16 4, vpkd3d128 float16_2 1, blrl 1) - fdc35ee.
- Unsupported instructions (110 sites: vaddsws 46, lhbrx 18, vpkshss 14, vminsw 14, vavgsw 8, vandc 5,
  vsel128 2, vctuxs 1, stvebx 1, mulhdu 1) - implemented on the pacman-instructions branch, see above.
- Function boundaries: all 5 switch-containing functions defined in pacman.toml; no boundary errors.
- Missed jump table at 820ACD1C (bctr): manual switch_tables.toml entry works - base 0x820ACCF8
  (the lis r12 after the bounds check, XenonAnalyse's convention), r30, 9 cases.
