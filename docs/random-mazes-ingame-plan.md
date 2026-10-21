# In-game random-mazes toggle - plan (2026-10-21, not started; nothing built or changed yet)

Owner's design: on the main menu, R3 toggles random mazes; while on, the menu is tinted (red or rainbow
instead of blue). Championship then starts random or normal. Random runs record NO high scores (owner's
choice: "secret bonus mode", runs can't be replicated). The startup option / random-mazes.bat stays.

## Pieces
1. Runtime maze order (core). The game reads gamedata1.bin once at boot, so swapping the file isn't enough:
   hook the code that picks "maze for stage N" and remap N through a permutation while the toggle is on
   (new permutation per run start; keep stage 1).
2. R3 + menu detection + tint. Controller hook exists (autodemo.cpp, 0x820A5D2C after XInputGetState).
   Need a "main menu is showing" signal. Tint: recolour sprite draws (sub_8213AB98 / sub_8213AD30 set per-
   sprite constants device+0x740..0x75C) only while on the menu.
3. No scores in random mode: find the save write (XamContent / file write of nmGameSave1\savegame.bin) or the
   high-score insert, and skip it while random is on. Back up the owner's save before any testing.

## Findings so far (piece 1)
- data\gamedata1.bin layout (2224 lines): lines 0-63 settings (key,value); lines 65-1582 = 46 maze grids of
  33 rows each, stored back to back with no names (top/bottom wall rows "w,w,w..."); 1583-1604 stage list 1
  (22 rows) + "m_,25,37"; 1612-1633 stage list 2; 1647+ more settings (eatstop, dualmap, speedbaserate...);
  1925-2155 another 7 grids; ~2162 a dev directory listing of m_i_mapXX.csv files.
- Stage rows: "label,m_i_mapXX,a,b" with a in 0..3 and b = 24 for every row; the label means
  "start, end grid". The m_i_map name may be only a comment and a/b the real data - OPEN QUESTION.
  The startup shuffle moves whole rows, and the owner saw new maze orders, so the game does depend on row
  order somehow. First step next time: find out how the grids and stage rows are turned into stages.
- The loader: sub_82136D30 loads all gamedata*.bin (string refs 0x82136EC0 etc.), object size 0x110 via
  sub_82139128 (constructor, takes path); settings lookup by key: sub_82140160 (walks sections from global
  0x821F4C60 -> +0xC -> +0x14, rows +0x18 next, key at +0x30), value read sub_821402a8.
- No "m_i_map" string in the exe, so map names aren't looked up by name in code.

- Checked 2026-10-21: "m_i_map" appears only in the gamedata*.bin files (no separate maze files, not in the
  exe) -> the names are comments. All 46 grids at lines 65-1582 are DIFFERENT from each other, even for
  stages sharing a name (stage 2 and 12 = m_i_map01), so grids are not one per name. Likely the grids are
  addressed by position (46 = 2 x 23: per stage and half?) and/or the a,b numbers. Since shuffling whole
  rows visibly changed the mazes, the row's position or the a value must matter - test by shuffling ONLY
  the a column (or only names) at startup and see what changes.

## Next steps
a. Find the stage-list parser: grep the generated code / disassemble the constructor sub_82139128 and the
   CSV row parser for how rows without a known key (stage rows, grid rows) are stored.
b. Find who reads the stage list during play (watch reads of that table, or log at stage change with a
   hook), and hook there with a remap table.
c. Then pieces 2 and 3. Test against a copy of userdata, never the owner's save.
