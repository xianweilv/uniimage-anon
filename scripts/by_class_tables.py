#!/usr/bin/env python3
1. policystorage (GB)（ingestnew_obj_comp + recipe_gz）
2. Q2 rebuildthroughput (MB/s)
"""
import json
import os
from collections import defaultdict

R = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")

m1 = json.load(open(os.path.join(R, "m1_m3.json"), encoding="utf-8"))

def cls_of(rec):
    p = rec["file"].replace("\\", "/")
    if "/images/base/" in p:
        return "base"
    if "/images/ai/" in p:
        return "ai"
    return "app"

rows = {}
nd = defaultdict(float)
nat = defaultdict(float)
seen = set()
for r in m1["policies"]["hybrid"]:
    c = cls_of(r)
    nd[c] += r["comp"]
    if r["blob_sha"] not in seen:
        seen.add(r["blob_sha"])
        nat[c] += r["comp"]
rows["no-dedup"] = dict(nd)
rows["NAT"] = dict(nat)
for pol in [p for p in ["file", "fixblock", "chunk16k", "chunk16kb", "hybrid"] if p in m1["policies"]]:
    st = defaultdict(float)
    for r in m1["policies"][pol]:
        st[cls_of(r)] += r.get("new_obj_comp", 0) + r.get("recipe_gz", 0)
    rows[pol] = dict(st)
dh = defaultdict(float)
for r in m1["policies"]["file"]:
    dh[cls_of(r)] += r.get("new_obj_raw", r.get("new_obj", 0)) if False else 0
ftot = sum(rows["file"].values())
fs = m1["file_summary"]
for c, v in rows["file"].items():
    dh[c] = fs["obj_raw_bytes"] * (v / ftot)
rows["DH(raw)"] = dict(dh)

print("=== Per-class stored (GB) ===")
hdr = "%-12s %8s %8s %8s %10s" % ("system", "base", "ai", "app", "total")
print(hdr)
for name in [n for n in ["no-dedup", "NAT", "DH(raw)", "file", "fixblock", "chunk16k", "chunk16kb", "hybrid"] if n in rows]:
    d = rows[name]
    t = sum(d.values())
    print("%-12s %8.2f %8.2f %8.2f %10.2f" % (
        name, d.get("base", 0) / 1e9, d.get("ai", 0) / 1e9,
        d.get("app", 0) / 1e9, t / 1e9))

log = defaultdict(float)
for r in m1["policies"]["hybrid"]:
    log[cls_of(r)] += r["tar"]
print("\n=== Uniimage per-class DR (logical/stored) ===")
for c in ["base", "ai", "app"]:
    print("%-6s logical=%7.2fGB stored=%6.2fGB DR=%.1f" % (
        c, log[c] / 1e9, rows["hybrid"][c] / 1e9, log[c] / rows["hybrid"][c]))

q2 = json.load(open(os.path.join(R, "q2_synth.json"), encoding="utf-8"))
recs = q2 if isinstance(q2, list) else q2.get("records", [])
print("\n=== Q2 per-class median throughput (MB/s) ===")
print("%-10s %8s %8s %8s" % ("engine", "base", "ai", "app"))
for mode in ["synth", "splice", "b4", "inflate"]:
    by = defaultdict(list)
    for r in recs:
        m = r["modes"].get(mode)
        if m and m.get("mbps"):
            by[cls_of(r)].append(m["mbps"])
    print("%-10s %8.1f %8.1f %8.1f" % (
        mode, sorted(by["base"])[len(by["base"]) // 2],
        sorted(by["ai"])[len(by["ai"]) // 2],
        sorted(by["app"])[len(by["app"]) // 2]))
