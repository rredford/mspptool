"""Compare a remake's run of GLITCHTEST.DAT with the expected results recorded from the original game.

Usage: python3 glitchcheck.py expected.jsonl remake.jsonl [--verbose]

Both files hold one JSON object per line: {"level": N, "title": "...", "ticks": [ {...}, ... ]}.
Each tick object (after tick n has run):
  n           tick number (1 = first tick after the level starts)
  status      "playing", "dead", "won", or "level data lost"
  chip        [x, y] of Chip
  chips_left  computer chips still needed
  monsters    monster list in order, each [tile code, x, y]
  digest      first 16 hex digits of SHA-1 over the 1024 top-layer tile codes followed by the 1024 bottom-layer codes
              (row by row, x fastest; tile codes as in the original level format)
  top/bottom  {square index: new tile code} for squares that changed this tick (index = y * 32 + x); optional, used only
              to explain a digest difference
A run stops early when the status stops being "playing". The remake must play each level from the start with the random
generator state 0 and deliver the key presses in manifest.json the same way (see key_semantics there).
Exit code 0 when every level matches."""
import json, sys

FIELDS = ('status', 'chip', 'chips_left', 'monsters', 'digest')

def load(path):
    return {r['level']: r for r in (json.loads(l) for l in open(path) if l.strip())}

def main(exp_path, got_path, verbose=False):
    exp, got = load(exp_path), load(got_path); bad = 0
    for lvl in sorted(exp):
        e = exp[lvl]
        if lvl not in got:
            print('level %3d %-28s MISSING' % (lvl, e['title'][:28])); bad += 1; continue
        et, gt = e['ticks'], got[lvl]['ticks']; first = None
        for i in range(max(len(et), len(gt))):
            if i >= len(et) or i >= len(gt):
                first = (i + 1, 'length', len(et), len(gt)); break
            for f in FIELDS:
                if et[i].get(f) != gt[i].get(f):
                    first = (et[i]['n'], f, et[i].get(f), gt[i].get(f)); break
            if first: break
        if first:
            bad += 1; n, f, a, b = first
            print('level %3d %-28s DIFFERS at tick %d in %s' % (lvl, e['title'][:28], n, f))
            if verbose:
                print('      expected: %s\n      remake:   %s' % (json.dumps(a)[:300], json.dumps(b)[:300]))
                if f == 'digest' and n - 1 < len(et) and n - 1 < len(gt):
                    for layer in ('top', 'bottom'):
                        ea, ga = et[n - 1].get(layer, {}), gt[n - 1].get(layer, {})
                        keys = sorted(set(ea) | set(ga), key=int)
                        d = [(int(k) % 32, int(k) // 32, ea.get(k), ga.get(k)) for k in keys if ea.get(k) != ga.get(k)]
                        if d: print('      %s-layer changes that differ (x, y, expected, remake): %s' % (layer, d[:12]))
        elif verbose:
            print('level %3d %-28s ok (%d ticks)' % (lvl, e['title'][:28], len(et)))
    print('\n%d of %d levels match' % (len(exp) - bad, len(exp)))
    return 1 if bad else 0

if __name__ == '__main__':
    if len(sys.argv) < 3: print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2], '--verbose' in sys.argv))
