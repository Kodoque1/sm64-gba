#!/usr/bin/env python3
"""Summarize the demo's per-frame stats lines (mgba_runner stdout)."""
import collections
import sys

rows = [l.split() for l in open(sys.argv[1]) if l.startswith("f ")]
d = [dict(zip(r[0::2], (int(v, 16) for v in r[1::2]))) for r in rows]
if not d:
    sys.exit("no frames")
vb = collections.Counter(x["v"] for x in d)
print(f"{len(d)} game frames; VBlanks per frame: " + ", ".join(f"{k}: {v}" for k, v in sorted(vb.items())))
print(f"  -> {100 * vb.get(2, 0) / len(d):.0f}% of frames at 30 fps")
byq = collections.defaultdict(list)
for x in d:
    byq[x["q"]].append(x)
for q, v in sorted(byq.items()):
    r = [x["r"] for x in v]
    print(f"  quality step {q}: {len(v):4d} frames, render mean {sum(r) // len(r):6d} max {max(r):6d} cycles")
levels = ["bob", "wf", "ccm", "bitdw"]
for lv in sorted(set(x.get("l", 0) for x in d)):
    sub = [x for x in d if x.get("l", 0) == lv]
    ok = sum(1 for x in sub if x["v"] == 2)
    print(f"  {levels[lv]:6s} {len(sub):4d} frames, {100 * ok / len(sub):5.1f}% at 30 fps, "
          f"quality steps used: {sorted(set(x['q'] for x in sub))}")
p = [x["p"] for x in d]
print(f"  Mario physics: mean {sum(p) // len(p)} max {max(p)} cycles")
