#!/usr/bin/env python3

- rootfs  data/rootfs_cache/<image>（q9 cache，373
- entry  data/images/<cls>/<image>/  config blob（sha256:*.json）
- file zlib-6 compression clen（ingest = objectcompressionsize）
- t_range = Σ clen/BW + n×RTT（file
- t_coal = Σ clen/BW + min(n,2)×RTT（object
"""
import json
import os
import statistics as st
import sys
import zlib
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_q4_modes import boot_file_set  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "data", "rootfs_cache")
IMAGES = os.path.join(ROOT, "data", "images")
OUT = os.path.join(ROOT, "results", "q4_range.json")

BW = 200e6 / 8
RTT = 0.005


def read_entry(cls, repo, tag):
    img_dir = os.path.join(IMAGES, cls,
                           repo.split("/")[-1] + "__" + tag)
    try:
        cands = [f for f in os.listdir(img_dir) if f.startswith("sha256:")]
    except OSError:
        return []
    for f in cands:
        try:
            cfg = json.load(open(os.path.join(img_dir, f)))
            e = cfg.get("config", {}).get("Entrypoint") or \
                cfg.get("config", {}).get("Cmd") or []
            if e:
                return e
        except Exception:
            continue
    return []


def main():
    idx = json.load(open(os.path.join(ROOT, "data", "layer_index.json")))
    images = {}
    for l in idx["layers"]:
        key = "%s__%s__%s" % (l["class"], l["repo"].replace("/", "_"), l["tag"])
        images[key] = (l["class"], l["repo"], l["tag"])
    rows = []
    skipped = 0
    for name, (cls, repo, tag) in sorted(images.items()):
        rootfs = os.path.join(CACHE, name)
        if not os.path.isdir(os.path.join(rootfs, "bin")) and \
           not os.path.isdir(os.path.join(rootfs, "usr")):
            skipped += 1
            continue
        entry = read_entry(cls, repo, tag)
        boot = boot_file_set(rootfs, entry)
        if not boot:
            skipped += 1
            continue
        tot_clen = 0
        n = 0
        for bf in sorted(boot):
            p = os.path.join(rootfs, bf)
            try:
                d = open(p, "rb").read()
            except OSError:
                continue
            co = zlib.compressobj(6, zlib.DEFLATED, 16 + 15)
            tot_clen += len(co.compress(d) + co.flush())
            n += 1
        if n == 0:
            skipped += 1
            continue
        rows.append({"image": name, "class": cls, "n_files": n,
                     "clen_bytes": tot_clen,
                     "t_range_s": tot_clen / BW + n * RTT,
                     "t_coal_s": tot_clen / BW + min(n, 2) * RTT})
    json.dump({"rows": rows, "bw_mbps": 200, "rtt_ms": 5, "skipped": skipped},
              open(OUT, "w"), indent=0)
    print("images measured:", len(rows), "skipped:", skipped)
    by = defaultdict(list)
    for r in rows:
        by[r["class"]].append(r)
    print("=== per-class medians (sub-stream range fetch, 200Mb/s + 5ms) ===")
    for c in ["base", "ai", "app"]:
        if by[c]:
            print("%-5s n=%3d  t_range=%.3fs  t_coal=%.3fs  clen_med=%dB" % (
                c, len(by[c]),
                st.median([r["t_range_s"] for r in by[c]]),
                st.median([r["t_coal_s"] for r in by[c]]),
                st.median([r["clen_bytes"] for r in by[c]])))


if __name__ == "__main__":
    main()
