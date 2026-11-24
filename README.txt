# Made with LLM, beware of slop. I carefully ensured it works well, but be aware.

MSPP tool - MSCC data viewer and stepper
=========================================

Runs the original Chip's Challenge (MSCC, CHIPS.EXE) and shows its live game data: both board
layers, the data after the board (what clone wires to row 32 reach), inventory, the monster,
slide, trap and clone lists, and a hex view and editor of every data block the game owns.
The game runs one tick at a time under this tool's control: play, pause, step, speed.

Files
-----
mspptool.exe   the tool (32-bit Windows program)
DATATAP.EXE    small 16-bit helper started next to the game; it reports where the game's data
               blocks are. Keep it in the same folder as mspptool.exe.
mspptool.ini   created by the tool: your settings.
run\           created by the tool: the working copy of the game it runs.
records\       created by the tool: data maps it records.
solutions\     the MSCC solution pack: one SET.mscc.tws per level set (pi, CCLP2, Jacques, Joshie,
               PATCH-TESTS), every record checked to win in MSCC. Type the level number, Load input,
               choose the .mscc.tws file, Run input: the record's own random seed is used.
glitchtest\    the glitch test pack: GLITCHTEST.DAT, an input file per level (inputs\), the tick-by-tick
               data map of every level (datamaps\), expected results and the checker.
source\        the C source of mspptool.exe and DATATAP.EXE.
datamap_viewer.html  data map viewer: open it in any web browser (no game, no Wine needed), then
               "Open data map" (or drop a .jsonl on the page). Step or play through the ticks,
               see both board layers, the state, the 96 data bytes after the board, the monster
               and slide lists and what changed on each tick; click a square for its history.
               "Compare with..." opens a second map of the same level and marks every difference
               (diff buttons jump between the ticks where they differ). Keys: left / right arrow
               = 1 tick (Shift = 10), Home / End, Space = play.

Requirements
------------
- Your own MSCC folder (CHIPS.EXE, CHIPS.DAT and the other game files). The tool checks that
  CHIPS.EXE is the version it knows. It never changes your folder: it copies the game into run\
  and patches only how that copy is driven, never the game rules:
    - the game's own timer is never started (2 bytes at 0x2D27, plus the two timer intervals at
      0x2D1B / 0x2D21), so every tick comes from the tool;
    - the game ignores losing focus and being minimized (1 byte at 0x3AE5, 3 bytes at 0x3CED /
      0x3CF2 / 0x3CF7, all in window-message handlers): no PAUSED screen, and clicking in the tool
      or changing speed cannot drop a tick. Game > Pause (F3) still pauses as usual.
- 16-bit Windows support:
    Linux / macOS: Wine (run "wine mspptool.exe"). Nothing else to set.
    32-bit Windows: works directly (NTVDM).
    64-bit Windows: install otvdm (winevdm) and choose otvdm.exe in Setup.
  Tested here under Wine only. On Windows, reading the game's memory has not been tested yet.

Your saved game
---------------
To start a chosen level the tool rewrites MSCC's save section in entpack.ini (current level and
the passwords of the level set). It keeps a copy of your section first and puts it back when the
tool closes. Close the tool normally (not by killing it) to get your save back.

Using it
--------
1. Setup...: choose CHIPS.EXE in your MSCC folder (and otvdm.exe on 64-bit Windows).
2. Level set...: any .DAT file (for example GLITCHTEST.DAT). Without one, the game's own CHIPS.DAT.
3. Type a level number, then "Launch / go to level". The game starts paused. "<" and ">" go to the
   previous / next level; "< Solved" and "Solved >" go to the previous / next level that has a
   solution in the loaded .tws file (then Run input plays it). Choosing a different level set
   unloads the input file, since a solution file belongs to one level set.
4. The game only moves when the tool sends ticks (1 tick = 1/10 s of MSCC time; Chip moves at most
   once per 2 ticks). Three ways to run it:
   - Play / Pause at a speed from x0.25 to max: real-time play, use the arrow keys as normal.
   - Speed "1 move per key": no timer. Every arrow key you press runs 2 ticks (one Chip move),
     Space in the game window waits 2 ticks. Good for careful play, tick by tick.
   - "Step 1 tick" / "Step 10 ticks": run that many ticks with no key. Chip only moves if you
     pressed an arrow key in the game window before stepping; monsters, sliding and the clock
     move as usual. With an input file running, Step plays the next ticks of the file.
   Keys: MSCC takes keys only while its own window is active (it pauses when it loses focus).
   Play, Step and OK bring the game window to the front, so just use the arrow keys. Arrow keys
   pressed in the tool window also bring the game to the front and are passed on.
   While the game window is in front: F5 = play / pause, F6 = step 1 tick, F7 = step 10 ticks.
   Every arrow key you play is logged per tick (for "Save keys as SuperCC JSON").
   Input files, Play and Step work even when the game window is not in front.
   Close any other MSCC window first: the tool only drives the game window it started itself.
5. "OK on dialog" presses the button of the game's death, info or win box. After a win MSCC goes on
   to the next level; the tool sends Level > Previous so you stay on the level you won. After a death
   MSCC restarts the same level: the tool's tick count and key log then start again from 0 (also when you press OK in
   the game window yourself).
6. Edit: inventory boxes + "Apply inventory"; "Any field" for every named value; click a board
   square, type a hex value and Write (top or bottom layer, chosen by the radio buttons).
7. Window: resize freely (smallest 1100x780). Drag the gap between the board and the right
   panel, and the gap between the data box and the list, to give either side more room; the
   tool remembers both and the window size. The data box and the lists keep your scroll
   position while the game runs.
8. Hex view...: every data block (data segment, board layers, data after the board, lists, and
   all raw blocks), in 4 KB pages, with Offset + Bytes + Write. Writes into the game's stack are
   refused, and the stack range is shown as "--".

Input files (Load input..., then Run input)
-------------------------------------------
Run input relaunches the level, sets the random seed ("Seed at start", default 0) and starts
playing the file at the chosen speed. Pause or Step stops it; Play continues. Three formats:
- MSPP text input (.txt), as in the glitch pack:
      level 128
      ticks 50
      seed 0
      start shift          (press Shift first to start the clock; "start auto" = only if no key at tick 0)
      10: N                (after 10 ticks have run, press Up)
      30: E N              (several keys in one tick are allowed)
      1: C3,2              (click the square 3 right, 2 down from Chip)
- SuperCC solution JSON (.json): one character per tick; u/d/l/r = a key on that tick, "-" = no
  key, capitals = the rest of a move; mouse clicks are supported.
- Tile World solution (.tws): holds many levels. After Load input the line next to Run input says
  whether the level in the Level box has a solution in the file ("Level 10: has a solution" or
  "NO solution in this file"); it updates as you type. Run input plays the level in the Level box.
  Tile World times are halved to game ticks (as the MSPP replayer does). For a .mscc.tws file the
  record's MSCC random seed is used; for other .tws files, the "Seed at start" box.

Recording
---------
Tick "Record data map" before Run input (or before the first Play / Step of live play). The tool
writes records\<name>_level<N>.jsonl: one JSON line per tick with the status, Chip, chips and
time left, keys and boots, the random generator state, the 96 data bytes after the board, the
monster and slide lists, and every board square that changed (the first line holds both full
layers). "Save keys as SuperCC JSON..." saves the keys of the current attempt as a SuperCC solution,
which this tool (and SuperCC) can load again. Its "Seed" is MSCC's random state when that attempt
started (after a death, a restart or a win the game keeps its random state, so it is usually not 0).
Every attempt starts like a fresh launch: when a level is restarted (by you, after a death or after
a win) the tool sets MSCC's tick counter back to 0, as a new game would have it, so a saved attempt
replays exactly.

Command line (no window)
------------------------
mspptool.exe /test GAMEDIR LEVELSET INPUTFILE OUTFILE [LEVEL]
runs an input file and writes its data map to OUTFILE. LEVEL is needed for .tws files.
