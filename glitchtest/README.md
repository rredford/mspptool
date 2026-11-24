# MS Ponder Puzzlers glitch tester

A 149-level pack of small test levels. Each level checks one or more rules from the MSPP behaviour spec. The pack comes with the expected result of every level, recorded from the original game, and a checker that compares a remake's run against it.

## Files

| File | What it is |
| --- | --- |
| `GLITCHTEST.DAT` | The pack, in the original level-file format: 131 test levels, then empty pad levels 132–149 (no Chip). |
| `manifest.json` | One entry per test level: title, the spec rules it covers, the key script, the number of ticks to run, and what to watch. |
| `expected.jsonl` | The original game's result for each level, tick by tick (format described in `glitchcheck.py`). |
| `glitchcheck.py` | Compares a remake's results with `expected.jsonl` and reports the first tick and field that differ per level. |

## Inputs and data maps

| Folder | What it is |
| --- | --- |
| `inputs/level_NNN.txt` | The key script of each level as an MSPP input file (level, ticks, seed, start rule, and `n: keys` lines). |
| `inputs/level_NNN.json` | The same script as a SuperCC solution (one character per tick; clicks as SuperCC click markers). |
| `datamaps/level_NNN.jsonl` | The original game's run of each level, tick by tick, recorded with mspptool: status, Chip, chips and time left, keys and boots, random state, the 96 data bytes after the board (row 32 = byte x), the monster and slide lists, and the board squares that changed (the first line holds both full layers). |

The input files load in mspptool (Load input, then Run input). Both formats reproduce `expected.jsonl` tick for tick on all levels. mspptool can record new data maps from any input, from a Tile World .tws solution, or from live play.

## How to run a remake against it

1. Load each test level from the start, with the random generator state set to 0.
2. Deliver the key presses from the manifest. `keys["n"]` means: after n ticks have run (n = 0 is right after the level starts, before tick 1), press these keys, each handled before the next tick. An entry `Cdx,dy` is a mouse click on the square at offset (dx, dy) from Chip's square at that moment (the original's 9×9 view always contains it). A tick is 1/10 s of game time.
3. Run for the level's `ticks`, or until the level ends (death, win, or the level data is lost). After each tick, write Chip's square, the status, chips left, the monster list in order, and a digest of both board layers.
4. Run `python3 glitchcheck.py expected.jsonl remake.jsonl --verbose`. Exit code 0 means every level matches.

## Notes

- **Deterministic.** The expected results were recorded twice from the original game and are identical, tick for tick.
- **Early ends are expected:** some levels end early on purpose: Chip dies (for example level 9, a block sliding back onto him; levels 95 and 96, the death-cause reset that does not save him; level 100, a time-out), level 34 loses its level data (rule S80), level 67 is won (the multiple tank glitch makes an exit), and level 128 is won (a data reset buries an exit under a socket, rule S100; level 129 is its control), and levels 130 and 131 are won (the same burial summons fire boots or flippers).
- **Behaviour only.** The pack contains level data, key scripts and observed results, nothing taken from the game's code.
- **Not covered by the pack:** held keys, menu commands, pausing, restarting, a different pack size (level count, termination), or a level that crashes the game on load (data size). These stay in the spec as written tests.
- **pgchip extension:** the ice-block rules (P1–P11) need the patched game and are not in this pack.
