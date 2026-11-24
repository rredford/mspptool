MSCC solution pack — 2026-11-23
================================

One file per level set: SET.mscc.tws. Each record is a solution that wins in the original MSCC with no
steering. Every record in the pack was replayed in a fresh game and won.

Format
------
A Tile World .tws file (MS ruleset), with one change. Record bytes 8-11 (Tile World's seed field) hold the
state of MSCC's own random generator (x*214013+2531011) when the level starts. Set the generator to that value
and play the moves; the level is won. A seed of 0 means the level wins from state 0.

Level files used
----------------
PATCH-TESTS, pi, Jacques, CCLP2: the same .dat files as before.
CHIPS: the original CHIPS.DAT of MSCC (149 levels), from the public CC1 Tile World solutions (public_CHIPS.dac.tws).
Joshie: the 1325-level Joshie.dat the user supplied (2026-11-16). Level numbers are that file's numbers.

Contents
--------
Set           Levels  As-is  Seed  Repaired/new  Left out
PATCH-TESTS       16     16     0             0         0
pi                42     31     1             5         5
Jacques          986    613   203            14       156
CCLP2            149    123    21             5         0
Joshie          1325    986   257             5        77
CHIPS            149    132    13             1         4
Total           2667   1901   495            30       242   (2425 solved)

As-is         The Tile World solution, unchanged, wins with seed 0.
Seed          The Tile World moves, unchanged, win with the seed in the record (seeds tried: 0-999, some up to 10,000).
Repaired/new  Either the Tile World moves edited near each death (waits, step-outs, changed turns), or a new move
              list (goal planner, search tools, or a player's solution checked in MSCC). Each wins with the seed
              in the record. pi: 8 (a player's SuperCC solution, 2 ticks moved), 12, 15, 33, 36.
              CHIPS: 45 MONSTER LAB (a player's SuperCC solution, one 2-tick wait added at tick 1230).

REPORT.tsv lists every level with its source and, for each one left out, the reason.

Why levels are left out (242)
-----------------------------
  4  CHIPS 23 BLOBNET, 126 BLOCK N ROLL, 127 SKELZIE, 133 BLOBDANCE: no seed in 0-999 wins and the goal
     planner has not found a route yet
148  random level; the seed search and repairs found no win (most die; some never finish)
 63  Tile World rejects the existing solution (bad old records)
 13  changed or new Joshie levels with no win in seeds 0-999 yet
 12  non-random level that does not win in MSCC with the Tile World moves
  2  no solution at all
pi left out: 9, 11, 37, 41 (needs a run with a deliberate death; it can't be one record), and 42.
pi "End of All Time" is not included.
