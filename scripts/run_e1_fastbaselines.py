#!/usr/bin/env python3

layereachsize ~50 layer， ~150 layer）layer
  - synth（UniImage
  - b4 z6 / z1 / ld1 / ld6 / ld12（inflate + compressionlayer
eachlayer --verify。output pushed blob size。
"""
import json
import os
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
M1 = os.path.join(ROOT, "results", "m1_m3.json")
SYNTH = os.path.join(ROOT, "src", "tools", "hd_synth")
STORE = os.path.join(ROOT, "data", "stores", "store_hybrid")
OUT = os.path.join(ROOT, "results", "e1_fastbaselines.json")

VARIANTS = [("synth", None), ("b4", "z6"), ("b4", "z1"),
            ("b4", "ld1"), ("b4", "ld6"), ("b4", "ld12")]

print_lock = threading.Lock()


def sh(cmd, timeout=7200):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        return None, r.stderr[:200]
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line), None
            except json.JSONDecodeError:
                pass
    return None, r.stdout[:150]


def main():
    m1 = json.load(open(M1))
    by = {}
    for r in m1["policies"]["hybrid"]:
        cls = "base"
        for c in ("ai", "app"):
            if "/%s/" % c in r["file"].replace("\\", "/"):
                cls = c
        by.setdefault(cls, []).append(r)
    sel = []
    for cls, rs in by.items():
        rs.sort(key=lambda r: r["tar"])
        k = max(1, len(rs) // 50)
        sel.extend(rs[::k][:50])
    tot_tar = sum(r["tar"] for r in sel) / 1e6
    tot_comp = sum(r["comp"] for r in sel) / 1e6
    print(f"subset={len(sel)} layers tar={tot_tar:.0f}MB comp={tot_comp:.0f}MB",
          flush=True)

    results = {"subset_layers": len(sel), "tar_mb": tot_tar,
               "comp_mb": tot_comp, "variants": {}}
    for mode, comp in VARIANTS:
        label = mode if comp is None else f"{mode}-{comp}"
        recs = []
        lock = threading.Lock()

        def one(r):
            cmd = [SYNTH, STORE, mode, r["recipe"], "--verify",
                   "--beta", "16384"]
            if comp:
                cmd += ["--comp", comp]
            d, err = sh(cmd)
            with lock:
                recs.append((r["tar"], d, err, r["comp"]))

        with ThreadPoolExecutor(max_workers=12) as ex:
            list(ex.map(one, sel))
        ok = [x for x in recs if x[1]]
        import statistics as st
        med = st.median([d["mbps"] for _, d, _, _ in ok])
        tsum = sum(t for t, d, _, _ in ok)
        mssum = sum(d["ms"] for _, d, _, _ in ok)
        outsum = sum(d["out"] for _, d, _, _ in ok)
        compsum = sum(c for _, d, _, c in ok)
        verifies = [d.get("verify") for _, d, _, _ in ok]
        row = {
            "n_ok": len(ok), "n": len(recs),
            "median_mbps": med,
            "byte_weighted_mbps": tsum / 1e6 / (mssum / 1000.0),
            "rel_size_byteweighted": outsum / compsum,
            "all_verify": all(v == "OK" for v in verifies),
        }
        results["variants"][label] = row
        print(f"{label:10s} n={row['n_ok']}/{row['n']} med={med:.1f} "
              f"bw={row['byte_weighted_mbps']:.1f}MB/s "
              f"size={row['rel_size_byteweighted']:.4f} "
              f"verify={'OK' if row['all_verify'] else 'FAIL'}", flush=True)

    json.dump(results, open(OUT, "w"), indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
