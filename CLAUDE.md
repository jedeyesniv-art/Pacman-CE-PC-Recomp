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

## Recompile command (from C:\PacManRecomp)
tools\xenonRecomp\build\XenonRecomp\XenonRecomp.exe config\pacman.toml tools\xenonRecomp\XenonUtils\ppc_context.h > recomp_log.txt 2>&1

## Known facts
- Code range ~0x82070000-0x821C0000 (~1.5 MB). Same compiler runtime as Sonic Unleashed:
  save/restore helper spacing matches SWA exactly.
- Save/restore helpers: savegprlr_14 0x820B8760, restgprlr_14 0x820B87B0, savefpr_14 0x820B9620,
  restfpr_14 0x820B966C, savevmx_14 0x8215B050, restvmx_14 0x8215B2E8, savevmx_64 0x8215B0E4, restvmx_64 0x8215B37C
- setjmp/longjmp: not located yet (omitted from config).

## Status / open issues (second recompile run, 2026-10-20)
Recompile completes cleanly (100%); the log contains only unrecognized-instruction warnings (110 sites).
1. Unsupported instructions in XenonRecomp (become debug traps), site counts from the 2026-10-20 run:
   vaddsws 46, lhbrx 18, vpkshss 14, vminsw 14, vavgsw 8, vandc 5, vsel128 2, vctuxs 1, stvebx 1, mulhdu 1.
   Clusters: vaddsws/vminsw/vpkshss ~0x820D0CA4-0x820D174C and vavgsw+vaddsws ~0x820D7B78-0x820D7FCC
   (looks like audio mixing); lhbrx ~0x820AD5C4-0x820AD650, ~0x820CF8D8-0x820CFBE0 (looks like
   little-endian file parsing); vandc/vsel128 ~0x82076528-0x8207F588 (possibly a bitwise select pattern, unchecked);
   vctuxs+vsel128 0x820CE268; stvebx 0x820D33C4; mulhdu 0x821A3BA4. Check upstream PRs/forks first.
   Remember vector registers are stored fully reversed (see XenonRecomp README) - pack/select ops need care.
2. Next: implement the instructions above in XenonRecomp (vaddsws/lhbrx/vminsw/vpkshss cover 92 of 110 sites).

Resolved (verified 2026-10-20):
- Function boundaries: all 5 switch-containing functions defined in pacman.toml; no boundary errors.
- Missed jump table at 820ACD1C (bctr): manual switch_tables.toml entry works - base 0x820ACCF8
  (the lis r12 after the bounds check, XenonAnalyse's convention), r30, 9 cases.
