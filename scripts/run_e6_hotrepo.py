#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(ROOT, "data", "stores", "store_hybrid")
ING = os.path.join(ROOT, "src", "tools", "hd_ingest")
OUT = os.path.join(ROOT, "results", "e6_hotrepo.json")

NEW = [
    ("app", "library/nginx", "1.27.4"),
    ("app", "library/postgres", "17.4"),
    ("app", "library/memcached", "1.6.38"),
    ("base", "library/python", "3.13.2-slim-bookworm"),
    ("base", "library/golang", "1.25.1-bookworm"),
    ("base", "library/alpine", "3.22.1"),
]


def main():
    from pull_dataset import pull_one
    rows = []
    for cls, repo, tag in NEW:
        r = pull_one(cls, repo, tag, os.path.join(ROOT, "data", "images"))
        if r[3] == "fail":
            print("PULL FAIL", repo, tag, r[3], flush=True)
            continue
        name = f"{repo.split('/')[-1]}__{tag}".replace(":", "-")
        d = os.path.join(ROOT, "data", "images", cls, name)
        layers = sorted(
            os.path.join(d, "layers", f)
            for f in os.listdir(os.path.join(d, "layers")) if f.endswith(".gz"))
        t0 = time.time()
        p = subprocess.run([ING, STORE, "hybrid"] + layers,
                           capture_output=True, text=True, timeout=3600)
        wall = time.time() - t0
        comp = 0
        hit = new = 0
        for l in p.stdout.splitlines():
            if l.startswith("{"):
                j = json.loads(l)
                comp += j.get("comp", 0)
                hit += j.get("hit_obj", 0) * 0
                new += j.get("new_obj_comp", 0)
                hit += j.get("sum_ref_comp", 0) - j.get("new_obj_comp", 0)
        tot_ref = new + hit
        rows.append({
            "repo": repo, "tag": tag, "layers": len(layers),
            "comp_mb": comp / 1e6, "wall_s": wall,
            "mbps": comp / 1e6 / wall,
            "dedup_hit_bytes_frac": hit / tot_ref if tot_ref else 0,
        })
        print(f"{repo}:{tag} {comp/1e6:.0f}MB {wall:.0f}s "
              f"{comp/1e6/wall:.1f}MB/s hit={hit/tot_ref*100 if tot_ref else 0:.0f}%",
              flush=True)
    import statistics as st
    if rows:
        summary = {
            "n": len(rows),
            "median_mbps": st.median([r["mbps"] for r in rows]),
            "mean_hit_frac": sum(r["dedup_hit_bytes_frac"] for r in rows)
            / len(rows),
        }
        print("SUMMARY:", json.dumps(summary), flush=True)
        json.dump({"images": rows, "summary": summary}, open(OUT, "w"),
                  indent=0)
        print("saved", OUT)


if __name__ == "__main__":
    main()
