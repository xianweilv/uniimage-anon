#!/usr/bin/env python3

A)  GC object:
   - image recipe  object -> image
   - : random / ( Zipf )
   -  10% image:  vs (imageobject),
      100%（
B) GC :
   - mark : rebuild recipe） ×3
   - sweep :  unlink throughput（objectfilefile
   - : 24 layersynthesislatency，GC / comparison
C) pinned storage:
   - pin : none /  repo  tag / 25% / 50% / image
   - storage = objectstorage + recipe + Σ pinned layercompression blob（layerdeduplication）
   -  B1（ UniImage unpinned
"""
import argparse
import json
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recipe_reader import load_recipe

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "data", "images")
STORE_HY = os.path.join(ROOT, "data", "stores", "store_hybrid")
SYNTH = os.path.join(ROOT, "src", "tools", "hd_synth")
M1 = os.path.join(ROOT, "results", "m1_m3.json")
OUT = os.path.join(ROOT, "results", "q7_gc.json")


def sh_json(cmd, timeout=3600):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        return None, r.stderr[:300]
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line), None
            except json.JSONDecodeError:
                continue
    return None, r.stdout[:200]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    m1 = json.load(open(M1))
    rec_by_sha = {}
    rec_comp = {}
    for r in m1["policies"]["hybrid"]:
        if "recipe" in r and r.get("blob_sha"):
            rec_by_sha[r["blob_sha"]] = r
        if "recipe" in r:
            rec_comp[r["recipe"]] = r.get("comp", 0)

    images = {}          # (repo,tag) -> {class, recs:[rsha]}
    for cls in os.listdir(IMAGES):
        clsdir = os.path.join(IMAGES, cls)
        if not os.path.isdir(clsdir):
            continue
        for name in sorted(os.listdir(clsdir)):
            ip = os.path.join(clsdir, name, "index.json")
            if not os.path.exists(ip):
                continue
            info = json.load(open(ip))
            recs = []
            for lay in info["layers"]:
                dg = lay["digest"].split(":")[-1].split(".tar.gz")[0].split(".")[0]
                r = rec_by_sha.get(dg)
                if r:
                    recs.append(r["recipe"])
            images[(info["repo"], info["tag"])] = {"class": cls, "recs": recs}

    t0 = time.time()
    obj_refs = {}        # sha -> set(img_key)
    obj_size = {}
    img_objs = {}        # img_key -> {sha: size}
    recipe_bytes = 0
    for key, img in images.items():
        s = {}
        for rsha in img["recs"]:
            p = os.path.join(STORE_HY, "recipes", rsha[:2], rsha)
            recipe_bytes += os.path.getsize(p)
            try:
                rec = load_recipe(STORE_HY, rsha)
            except Exception:
                continue
            for (sha, raw_len, crc) in rec.objs:
                obj_refs.setdefault(sha, set()).add(key)
                if sha not in obj_size:
                    op = os.path.join(STORE_HY, "objects", sha[:2], sha)
                    obj_size[sha] = os.path.getsize(op)
                if sha not in s:
                    s[sha] = obj_size[sha]
        img_objs[key] = s
    graph_build_s = time.time() - t0
    total_obj_bytes = sum(obj_size.values())
    uni_storage = total_obj_bytes + recipe_bytes

    print(f"images={len(images)} objects={len(obj_refs)} "
          f"uni_storage={uni_storage/1e9:.3f}GB (obj {total_obj_bytes/1e9:.3f} + "
          f"recipe {recipe_bytes/1e9:.3f}) graph_build={graph_build_s:.1f}s",
          flush=True)

    milestones = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]

    def delete_sim(keys_ordered):
        remaining = set(images.keys())
        dead = set()
        seen_naive = set()
        naive_footprint = 0
        live_bytes = total_obj_bytes
        rows = []
        n = len(keys_ordered)
        mi = 0
        for i, key in enumerate(keys_ordered, 1):
            remaining.discard(key)
            for sha, sz in img_objs[key].items():
                if sha not in seen_naive:
                    seen_naive.add(sha)
                    naive_footprint += sz
                if sha not in dead and not (obj_refs[sha] & remaining):
                    dead.add(sha)
                    live_bytes -= sz
            while mi < len(milestones) and i >= milestones[mi] * n:
                reclaimed = total_obj_bytes - live_bytes
                rows.append({
                    "deleted_frac": milestones[mi],
                    "live_objects": len(obj_refs) - len(dead),
                    "live_bytes": live_bytes,
                    "live_vs_uni": live_bytes / uni_storage,
                    "reclaimed_bytes": reclaimed,
                    "reclaimed_vs_objstore": reclaimed / total_obj_bytes,
                    "naive_expected_bytes": naive_footprint,
                    "reclaim_over_naive": reclaimed / max(1, naive_footprint),
                })
                mi += 1
        return rows

    random.seed(args.seed)
    keys = list(images.keys())
    random.shuffle(keys)
    sim_random = delete_sim(keys)

    cls_rank = {"base": 0, "app": 1, "ai": 2}
    pop = sorted(keys, key=lambda k: (cls_rank.get(images[k]["class"], 3),
                                      random.random()))
    sim_cold = delete_sim(pop[::-1])

    for row in sim_random:
        print(f"rand del={row['deleted_frac']:.0%} live={row['live_bytes']/1e9:6.3f}GB "
              f"({row['live_vs_uni']*100:5.1f}%) reclaimed={row['reclaimed_bytes']/1e9:6.3f}GB "
              f"naive={row['naive_expected_bytes']/1e9:6.3f}GB", flush=True)

    mark_times = []
    for _ in range(3):
        t0 = time.time()
        for key, img in images.items():
            for rsha in img["recs"]:
                try:
                    load_recipe(STORE_HY, rsha)
                except Exception:
                    pass
        mark_times.append(time.time() - t0)

    scratch = os.path.join(ROOT, "data", "gc_scratch")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    sample = list(obj_size)[:10000]
    for sha in sample:
        src = os.path.join(STORE_HY, "objects", sha[:2], sha)
        dst = os.path.join(scratch, sha)
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy(src, dst)
    t0 = time.time()
    for sha in sample:
        os.unlink(os.path.join(scratch, sha))
    sweep_s = time.time() - t0
    sweep_bytes = sum(obj_size[s] for s in sample)
    shutil.rmtree(scratch, ignore_errors=True)

    probe = sorted([r for r in rec_by_sha.values() if r.get("tar", 0) > 20e6],
                   key=lambda r: r["tar"])[:24]

    def synth_round():
        lat = []
        for r in probe:
            t0 = time.time()
            d, err = sh_json([SYNTH, STORE_HY, "synth", r["recipe"], "--verify"])
            lat.append(time.time() - t0)
            if not d or d.get("verify") != "OK":
                print("probe verify fail", err, flush=True)
        return lat

    lat_alone = synth_round()

    stop = threading.Event()

    def gc_loop():
        while not stop.is_set():
            for key, img in images.items():
                if stop.is_set():
                    return
                for rsha in img["recs"]:
                    if stop.is_set():
                        return
                    try:
                        load_recipe(STORE_HY, rsha)
                    except Exception:
                        pass

    th = threading.Thread(target=gc_loop, daemon=True)
    th.start()
    lat_gc = synth_round()
    stop.set()
    th.join()

    fg = {
        "mark_times_s": mark_times,
        "mark_ms_per_gb": statistics.mean(mark_times) / (uni_storage / 1e9) * 1000,
        "sweep_files": len(sample), "sweep_s": sweep_s,
        "sweep_unlinks_per_s": len(sample) / sweep_s,
        "sweep_mbps": sweep_bytes / sweep_s / 1e6,
        "probe_layers": len(probe),
        "probe_alone_p50": statistics.median(lat_alone),
        "probe_alone_p99": sorted(lat_alone)[max(0, int(len(lat_alone) * 0.99) - 1)],
        "probe_gc_p50": statistics.median(lat_gc),
        "probe_gc_p99": sorted(lat_gc)[max(0, int(len(lat_gc) * 0.99) - 1)],
    }
    print(f"mark={statistics.mean(mark_times):.2f}s ({fg['mark_ms_per_gb']:.0f}ms/GB) "
          f"sweep={len(sample)/sweep_s:.0f} unlink/s "
          f"probe p50 {fg['probe_alone_p50']:.2f}s -> {fg['probe_gc_p50']:.2f}s (with GC)",
          flush=True)

    b1_bytes = m1["layer_dedup_bytes"]
    repos = {}
    for (repo, tag) in images:
        repos.setdefault(repo, []).append(tag)

    def tag_key(t):
        nums = [int(x) if x.isdigit() else x
                for x in re.findall(r"\d+|\D+", t)]
        return ([(0, x) if isinstance(x, int) else (1, x) for x in nums], t)

    latest = set()
    for repo, tags in repos.items():
        latest.add((repo, sorted(tags, key=tag_key)[-1]))
    random.seed(args.seed)
    allk = list(images.keys())
    random.shuffle(allk)
    pinned_sets = {
        "none": set(),
        "latest_tag": latest,
        "pct25": set(allk[:len(allk) // 4]),
        "pct50": set(allk[:len(allk) // 2]),
        "all": set(allk),
    }

    pinned_rows = []
    for pname, pkeys in pinned_sets.items():
        seen = set()
        blob_bytes = 0
        for key in pkeys:
            for rsha in images[key]["recs"]:
                if rsha in seen:
                    continue
                seen.add(rsha)
                blob_bytes += rec_comp.get(rsha, 0)
        tot = uni_storage + blob_bytes
        pinned_rows.append({
            "set": pname, "pinned_layers": len(seen),
            "pinned_blob_bytes": blob_bytes,
            "total_bytes": tot,
            "vs_b1": tot / b1_bytes,
            "vs_uni": tot / uni_storage,
        })
        print(f"pinned={pname:10s} layers={len(seen):3d} blob={blob_bytes/1e9:7.3f}GB "
              f"total={tot/1e9:7.3f}GB vs_b1={tot/b1_bytes:.3f} "
              f"vs_uni={tot/uni_storage:.3f}", flush=True)

    results = {
        "config": {"seed": args.seed, "n_images": len(images),
                   "n_objects": len(obj_refs),
                   "uni_storage_bytes": uni_storage,
                   "obj_bytes": total_obj_bytes,
                   "recipe_bytes": recipe_bytes,
                   "graph_build_s": graph_build_s,
                   "b1_bytes": b1_bytes},
        "gc_random": sim_random,
        "gc_cold_first": sim_cold,
        "gc_overhead": fg,
        "pinned": pinned_rows,
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(results, open(OUT, "w"), indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
