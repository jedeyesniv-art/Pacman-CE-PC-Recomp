# Pac-Man Championship Edition (Xbox 360 XBLA) - static recompilation

Hobby project: native PC port of Pac-Man CE using XenonRecomp. Windows, VS 2026 (v18), Clang 22.
Game data comes from the owner's own console dump and is never committed or shared.

## Layout (C:\PacManRecomp)
- private\default.xex      original executable (do not modify)
- private\default.exe      decrypted/decompressed PE basefile (from xextool -b); ImageBase 0x82000000
- config\pacman.toml       main XenonRecomp config
- config\switch_tables.toml  jump tables from XenonAnalyse (+ manual additions)
- ppc\                     generated C++ (~30 files). NEVER read wholesale - search it.
- tools\xenonRecomp\       XenonRecomp source + build\
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

## Status / open issues (third recompile run, 2026-10-20)
Recompile completes cleanly (100%) with an empty log: no unrecognized instructions remain.
tools\xenonRecomp is on local branch pacman-instructions (on top of upstream ddd128b):
  a8e7b38 cherry-picked upstream PR #181 (lhbrx, vandc, vsel128, mulhd, eqv, bso/bns, vpkswss)
  da486ee our additions: mulhdu, vaddsws, vavgsw, vminsw, vpkshss(128), vctuxs/vcfpuxws128, stvebx
          (new helpers simde_mm_adds_epi32 / simde_mm_avg_epi32 / simde_mm_vctuxs in ppc_context.h,
          tested vs scalar reference; vpkshss/vctuxs adapted from upstream PR #60, which is stale/unmergeable)
Remember vector registers are stored fully reversed (see XenonRecomp README) - pack/select ops need care.
1. Remaining silent traps: XenonRecomp emits __builtin_debugtrap() for some recognized instructions without
   logging them. 8 sites in generated code: vcmpbfp x2 (ppc_recomp.15.cpp), vpkd3d128/vupkd3d128 with
   unsupported pack types x5 (ppc_recomp.2.cpp, ppc_recomp.8.cpp), blrl x1 (ppc_recomp.24.cpp; indirect call).
   Find them with: grep -n -B3 __builtin_debugtrap ppc/*.cpp
2. Not yet done: compiling the generated ppc\ code; nothing has run in-game; new instructions untested at runtime.

Resolved (verified 2026-10-20):
- Unsupported instructions (110 sites: vaddsws 46, lhbrx 18, vpkshss 14, vminsw 14, vavgsw 8, vandc 5,
  vsel128 2, vctuxs 1, stvebx 1, mulhdu 1) - implemented on the pacman-instructions branch, see above.
- Function boundaries: all 5 switch-containing functions defined in pacman.toml; no boundary errors.
- Missed jump table at 820ACD1C (bctr): manual switch_tables.toml entry works - base 0x820ACCF8
  (the lis r12 after the bounds check, XenonAnalyse's convention), r30, 9 cases.
