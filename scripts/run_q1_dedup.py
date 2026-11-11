#!/usr/bin/env python3

 M1  ingest  + recipe object
  - no-dedup / layer-dedup / file-level(compression) / fixblock(SimEnc) / hybrid(UniImage)
  - filefile policyobject
  - base/ai/app）
  - over-time）
  -  CDF

Usage: python3 scripts/run_q1_dedup.py
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recipe_reader import load_recipe

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")
OUT = os.path.join(RES, "q1_dedup.json")


def obj_size(store, sha):
    p = os.path.join(store, "objects", sha[:2], sha)
    try:
        return os.path.getsize(p)
    except OSError:
        return 0


def main():
    m1 = json.load(open(os.path.join(RES, "m1_m3.json")))
    idx = json.load(open(os.path.join(ROOT, "data", "layer_index.json")))
    layers = idx["layers"]

    logical_tar = 0
    comp_total = 0
    seen_blob = set()
    layer_dedup_bytes = 0
    for l in layers:
        logical_tar += 0   #  ingest
    hyb = m1["policies"]["hybrid"]
    file_pol = m1["policies"].get("file", [])
    fb = m1["policies"].get("fixblock", [])

    logical_tar = sum(r["tar"] for r in hyb)
    comp_total = sum(r["comp"] for r in hyb)
    for l in layers:
        if l["blob_sha"] not in seen_blob:
            seen_blob.add(l["blob_sha"])
            layer_dedup_bytes += l["size"]

    result = {
        "n_layers": len(hyb),
        "logical_tar_bytes": logical_tar,
        "compressed_total_bytes": comp_total,
        "no_dedup": {"bytes": comp_total},
        "layer_dedup": {"bytes": layer_dedup_bytes,
                        "unique_layers": len(seen_blob),
                        "logical_dr": logical_tar / layer_dedup_bytes,
                        "physical_dr": comp_total / layer_dedup_bytes},
    }

    for pol, recs in (("file", file_pol), ("fixblock", fb), ("hybrid", hyb)):
        if not recs:
            continue
        summ = m1.get(f"{pol}_summary", {})
        total = summ.get("obj_comp_bytes", 0) + summ.get("recipe_bytes", 0)
        raw = summ.get("obj_raw_bytes", 0)
        result[pol] = {
            "stored_bytes": total,
            "obj_raw_bytes": raw,
            "n_objects": summ.get("obj_unique", 0),
            "logical_dr": logical_tar / total,
            "physical_dr": comp_total / total,
        }

    if file_pol:
        result["file_ceiling"] = {
            "unique_raw_bytes": m1["file_summary"]["obj_raw_bytes"],
            "logical_dr": logical_tar / m1["file_summary"]["obj_raw_bytes"],
        }

    store = os.path.join(ROOT, "data", "stores", "store_hybrid")
    # layer -> (class, repo, tag)
    def normp(f):
        return os.path.normpath(f.replace("\\", "/"))
    meta = {}
    for l in layers:
        meta[normp(l["path"])] = (l["class"], l["repo"], l["tag"])

    order = []   # (file, class, repo, new_obj_bytes, new_recipe, tar, comp)
    for r in hyb:
        key = normp(r["file"])
        if key not in meta:
            key2 = os.path.normpath(os.path.join(ROOT, r["file"]))
            key2 = key2.replace(ROOT + os.sep, "").replace(os.sep, "/")
            key = key2
        cls, repo, tag = meta.get(key, ("?", "?", "?"))
        order.append((r, cls, repo, tag))

    seen = set()
    by_class = {}
    by_repo = {}
    over_time = []
    cum_store = 0
    cum_logical = 0
    cum_layer_dedup = 0
    seen_blob2 = set()
    for i, (r, cls, repo, tag) in enumerate(order):
        new_bytes = r.get("new_obj_comp", 0)
        rec_bytes = r.get("recipe_gz", 0)
        cum_store += new_bytes + rec_bytes
        cum_logical += r["tar"]
        if r["blob_sha"] not in seen_blob2:
            seen_blob2.add(r["blob_sha"])
            cum_layer_dedup += r["comp"]
        if (i + 1) % max(1, len(order) // 40) == 0 or i == len(order) - 1:
            over_time.append({
                "frac": (i + 1) / len(order),
                "uni_dr": cum_logical / cum_store if cum_store else 0,
                "layer_dr": cum_logical / cum_layer_dedup if cum_layer_dedup else 0,
            })
        d = by_class.setdefault(cls, {"logical": 0, "stored": 0})
        d["logical"] += r["tar"]
        d["stored"] += new_bytes + rec_bytes
        d = by_repo.setdefault(repo, {"logical": 0, "stored": 0, "class": cls})
        d["logical"] += r["tar"]
        d["stored"] += new_bytes + rec_bytes

    result["by_class"] = {k: {"logical": v["logical"], "stored": v["stored"],
                              "dr": v["logical"] / v["stored"] if v["stored"] else 0}
                          for k, v in by_class.items()}
    result["by_repo"] = {k: {"logical": v["logical"], "stored": v["stored"],
                             "dr": v["logical"] / v["stored"] if v["stored"] else 0,
                             "class": v["class"]}
                         for k, v in by_repo.items()}
    result["over_time"] = over_time

    with open(OUT, "w") as f:
        json.dump(result, f, indent=0)
    print("saved", OUT)

    print("\n=== Q1  ===")
    print(f"layers={result['n_layers']} logical={logical_tar/1e9:.2f}GB "
          f"comp={comp_total/1e9:.2f}GB")
    print(f"no-dedup      : {comp_total/1e9:.3f}GB  DR(log)={logical_tar/comp_total:.2f}")
    print(f"layer-dedup   : {layer_dedup_bytes/1e9:.3f}GB  "
          f"DR(log)={logical_tar/layer_dedup_bytes:.2f}")
    for pol in ("file", "fixblock", "hybrid"):
        if pol in result:
            print(f"{pol:12s} : {result[pol]['stored_bytes']/1e9:.3f}GB  "
                  f"DR(log)={result[pol]['logical_dr']:.2f} "
                  f"DR(phys)={result[pol]['physical_dr']:.2f} "
                  f"objs={result[pol]['n_objects']}")
    if "file_ceiling" in result:
        fc = result["file_ceiling"]
        print(f"file ceiling  : unique_raw={fc['unique_raw_bytes']/1e9:.3f}GB "
              f"DR={fc['logical_dr']:.2f}")
    print("\nby class:")
    for k, v in sorted(result["by_class"].items()):
        print(f"  {k}: DR={v['dr']:.2f} (logical {v['logical']/1e9:.2f}GB)")
    print("by repo (top/bottom 5):")
    repos = sorted(result["by_repo"].items(), key=lambda kv: kv[1]["dr"])
    for k, v in repos[:5] + repos[-5:]:
        print(f"  {k}: DR={v['dr']:.2f} ({v['class']})")


if __name__ == "__main__":
    main()
