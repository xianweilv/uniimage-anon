#!/usr/bin/env python3
"""M1 + M3 experiment（paper §III Mot-1 / Mot-3）

M1: storage —— all D2 layerpolicy store，
    storageobject+recipe）
      S1 no-dedup baseline       = Σ compressionlayersize
      S2 layer-dedup             =  blob_sha layersize（ index
      S3 fixblock-512K (SimEnc)
      S4 file-level (DupHunter, compressionobject——compressionstorage
         unique_raw )
      S5 hybrid (UniImage, sigma=1K theta=64K CDC64K)
    : chunk8k/chunk16k used for M3 compression

M3: compression —— eachlayer: whole-layer gzip L6  vs objectcompression
    (file/chunk8k/chunk16k/hybrid)，output coding penalty。

Usage: python3 scripts/run_m1_m3.py [--limit N] [--policies hybrid,file,chunk8k,chunk16k,fixblock]
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INGEST = os.path.join(ROOT, "src", "tools", "hd_ingest")
SYNTH = os.path.join(ROOT, "src", "tools", "hd_synth")
IDX = os.path.join(ROOT, "data", "layer_index.json")
OUT = os.path.join(ROOT, "results", "m1_m3.json")


def run_ingest(store, policy, paths, extra=(), jobs=8):
    import concurrent.futures as cf
    recs = []
    shards = [paths[i::jobs] for i in range(min(jobs, len(paths)))] if paths else []

    def one(shard):
        out = []
        B = 100
        for i in range(0, len(shard), B):
            batch = shard[i:i + B]
            try:
                r = subprocess.run([INGEST, store, policy, *extra] + batch,
                                   capture_output=True, text=True, timeout=100000)
            except subprocess.TimeoutExpired:
                print("INGEST TIMEOUT", flush=True)
                continue
            for line in r.stdout.splitlines():
                line = line.strip()
                if line.startswith("{"):
                    try:
                        out.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
            if r.returncode != 0:
                print("INGEST FAIL:", r.stderr[:200], flush=True)
        return out

    with cf.ThreadPoolExecutor(max_workers=len(shards) or 1) as ex:
        for res in ex.map(one, shards):
            recs.extend(res)
    return recs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0, help="eachlayer0=all）")
    ap.add_argument("--policies", default="hybrid,file,chunk8k,chunk16k,fixblock")
    ap.add_argument("--outdir", default=os.path.join(ROOT, "data", "stores"))
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()

    idx = json.load(open(IDX))
    layers = idx["layers"]
    if args.limit:
        by_cls = {}
        for l in layers:
            by_cls.setdefault(l["class"], []).append(l)
        layers = []
        for cls, ls in by_cls.items():
            layers.extend(ls[:args.limit])
    paths = [os.path.join(ROOT, l["path"]) for l in layers]
    print(f"{len(paths)} layers, "
          f"{sum(os.path.getsize(p) for p in paths)/1e9:.2f} GB compressed")

    os.makedirs(args.outdir, exist_ok=True)
    result = {
        "n_layers": len(paths),
        "no_dedup_bytes": sum(os.path.getsize(p) for p in paths),
        "policies": {},
        "layer_records": {},
    }

    seen = set()
    layer_dedup = 0
    for l in layers:
        if l["blob_sha"] not in seen:
            seen.add(l["blob_sha"])
            layer_dedup += l["size"]
    result["layer_dedup_bytes"] = layer_dedup
    result["layer_dedup_unique"] = len(seen)

    for pol in args.policies.split(","):
        store = os.path.join(args.outdir, f"store_{pol}")
        os.system(f"rm -rf {store}")
        extra = []
        if pol == "fixblock":
            extra = ["--theta", str(512 * 1024)]
        print(f"[ingest] policy={pol} ...", flush=True)
        recs = run_ingest(store, pol, paths, extra, jobs=args.jobs)
        result["policies"][pol] = recs
        stats_tool = os.path.join(ROOT, "src", "tools", "store_stats")
        r = subprocess.run([stats_tool, store], capture_output=True, text=True, timeout=600)
        try:
            st = json.loads(r.stdout.strip())
            result[f"{pol}_summary"] = {
                "obj_unique": st["n_obj"],
                "obj_raw_bytes": st["obj_raw"],
                "obj_comp_bytes": st["obj_comp"],
                "recipes_unique": st["n_recipes"],
                "recipe_bytes": st["recipe_bytes"],
                "blockmaps": st["n_blockmaps"],
            }
        except Exception:
            print("STATS FAIL:", r.stdout[:200], r.stderr[:200])
        pens = []
        for r in recs:
            if "comp" in r and r.get("tar", 0) > 0:
                pens.append({
                    "file": r["file"],
                    "comp": r["comp"],
                    "sum_ref_comp": r.get("sum_ref_comp", 0),
                    "recipe_gz": r.get("recipe_gz", 0),
                    "tar": r["tar"],
                })
        result["layer_records"][pol] = pens
        print(f"[done] {pol}: {len(recs)} layers", flush=True)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        json.dump(result, f, indent=0)
    print("saved", OUT)

    nd = result["no_dedup_bytes"]
    print(f"\n=== M1 storage{len(paths)} layer,  {nd/1e9:.3f} GB）===")
    print(f"no-dedup            : {nd/1e9:.3f} GB")
    print(f"layer-dedup         : {layer_dedup/1e9:.3f} GB ({layer_dedup/nd*100:.1f}%)")
    for pol in args.policies.split(","):
        s = result.get(f"{pol}_summary")
        if s:
            total = s["obj_comp_bytes"] + s["recipe_bytes"]
            print(f"{pol:18s} : {total/1e9:.3f} GB ({total/nd*100:.1f}%) "
                  f"objs={s['obj_unique']} raw={s['obj_raw_bytes']/1e9:.2f}GB")


if __name__ == "__main__":
    main()
