#!/usr/bin/env python3
import json
import os

R = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")


def load(name):
    return json.load(open(os.path.join(R, name), encoding="utf-8"))


def med(xs):
    s = sorted(xs)
    return s[len(s) // 2] if s else 0


print("=" * 60)
print("Q2 synthesis")
q2 = load("q2_synth.json")
recs = q2 if isinstance(q2, list) else q2.get("records", [])
print("n =", len(recs), "fields:", list(recs[0].keys())[:12] if recs else "-")
for eng in ["uni", "b2", "b4", "splice"]:
    key = eng + "_mbps"
    if recs and key in recs[0]:
        vals = [(r[key], r.get("tar", r.get("raw", 0))) for r in recs
                if r.get(key) is not None]
        medv = med([v[0] for v in vals])
        tot = sum(v[1] for v in vals)
        wt = sum(v[0] * v[1] for v in vals) / tot if tot else 0
        print(f"  {eng:7s} median={medv:7.1f} MB/s  weighted={wt:7.1f} MB/s")
    rk = eng + "_rel"
    if recs and rk in recs[0]:
        r = [x[rk] for x in recs if x.get(rk)]
        print(f"  {eng:7s} rel median={med(r):.3f}  p90={sorted(r)[int(len(r)*0.9)]:.3f}  max={max(r):.3f}")
# >1GB layer
big = [r for r in recs if r.get("tar", 0) > 1e9]
tot_big = sum(r.get("tar", 0) for r in recs)
print(f"  layers>1GB: {len(big)} ({sum(r['tar'] for r in big)/tot_big*100:.0f}% of bytes)")
for eng in ["uni", "b2"]:
    key = eng + "_mbps"
    if big and key in big[0]:
        v = [(r[key], r["tar"]) for r in big]
        print(f"  >1GB {eng}: weighted={sum(a*b for a,b in v)/sum(b for _,b in v):.1f} MB/s")

for name, keys in [
    ("q3_ingest_pull.json", None),
    ("q7_gc.json", None),
    ("q8_concurrency.json", None),
    ("e1_fastbaselines.json", None),
    ("e6_hotrepo.json", None),
]:
    print("=" * 60)
    print(name)
    d = load(name)
    print(json.dumps(d, indent=1)[:1500])

print("=" * 60)
print("Q4 modes")
q4 = load("q4_modes.json")
if isinstance(q4, dict):
    for k, v in q4.items():
        s = json.dumps(v)
        print(f"[{k}] {s[:400]}")

print("=" * 60)
print("Q5 compat")
q5 = load("q5_compat.json")
if isinstance(q5, dict):
    for k, v in q5.items():
        if isinstance(v, (int, float, str, bool)):
            print(f"{k}: {v}")
        elif isinstance(v, list):
            print(f"{k}: list[{len(v)}]")
        else:
            print(f"{k}: {json.dumps(v)[:200]}")

print("=" * 60)
print("Q6 cache")
q6 = load("q6_cache.json")
if isinstance(q6, dict):
    for k, v in q6.items():
        print(f"[{k}] {json.dumps(v)[:500]}")

print("=" * 60)
print("Q9 correctness")
q9 = load("q9_correctness.json")
if isinstance(q9, dict):
    for k, v in q9.items():
        if isinstance(v, list):
            print(f"{k}: list[{len(v)}] sample={json.dumps(v[:1])[:200]}")
        else:
            print(f"{k}: {json.dumps(v)[:300]}")

print("=" * 60)
print("R2 task")
r2 = load("r2_task.json")
if isinstance(r2, list):
    print("n =", len(r2), "fields:", list(r2[0].keys()) if r2 else "-")
    sp = [x.get("speedup") for x in r2 if x.get("speedup")]
    if sp:
        print(f"speedup: median={med(sp):.1f} p25={sorted(sp)[len(sp)//4]:.1f} p75={sorted(sp)[len(sp)*3//4]:.1f} min={min(sp):.2f} max={max(sp):.0f}")
elif isinstance(r2, dict):
    print(json.dumps(r2, indent=1)[:1200])
