#!/usr/bin/env python3

ingestthroughput:  M1  ingest layer t_wall / compression
  new+hit B1  = I/O  blob B2 = decompression+
   t_inflate

latency:  = max(rebuild, )）：
  t = Σ_layers max(tar_i / rebuild_mbps_i, blob_i' / BW) +
   blob' = B1 layer; rebuild = rebuildoutputsize）。
   100Mbps / 1Gbps / 10Gbps，cache（layerrebuild）
  trace-driven（X% cache）

: python3 scripts/run_q3_ingest_pull.py [--bw-mbps 100,1000,10000]
"""
import argparse
import json
import os
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "results", "q3_ingest_pull.json")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bw-mbps", default="100,1000,10000")
    ap.add_argument("--m1", default=os.path.join(ROOT, "results", "m1_m3.json"))
    ap.add_argument("--q2", default=os.path.join(ROOT, "results", "q2_synth.json"))
    ap.add_argument("--cache-hit-frac", default="0.0,0.5")
    args = ap.parse_args()

    m1 = json.load(open(args.m1))
    q2 = json.load(open(args.q2))
    recs = m1["policies"]["hybrid"]
    by_file = {r["file"]: r for r in recs}

    def bare(d):
        if not d:
            return d
        return d.split(":")[-1].split(".tar.gz")[0].split(".")[0]

    digest_map = {bare(r.get("blob_sha")): f for f, r in by_file.items()
                  if r.get("blob_sha")}
    q2_by_file = {l["file"]: l for l in q2}

    # ---------- ingestthroughput ----------
    total_comp = sum(r["comp"] for r in recs)
    total_new = sum(r.get("new_obj_comp", 0) for r in recs)
    total_ref = sum(r.get("sum_ref_comp", 0) for r in recs)
    t_wall = sum(r.get("t_wall_ms", 0) for r in recs)
    t_infl = sum(r.get("t_inflate_ms", 0) for r in recs)
    ingest = {
        "n_layers": len(recs),
        "comp_bytes": total_comp,
        "uniimage_wall_ms": t_wall,
        "uniimage_mbps": total_comp / 1e6 / (t_wall / 1000) if t_wall else 0,
        "dedup_hit_frac": 1 - (total_new / total_ref) if total_ref else 0,
        "b2_inflate_ms": t_infl,
        "b2_mbps_est": total_comp / 1e6 / (t_infl / 1000) if t_infl else 0,
    }
    print("=== ingest ===")
    for k, v in ingest.items():
        print(f"  {k}: {v}")

    images = {}
    IMAGES = os.path.join(ROOT, "data", "images")
    for cls in os.listdir(IMAGES):
        clsdir = os.path.join(IMAGES, cls)
        if not os.path.isdir(clsdir):
            continue
        for name in sorted(os.listdir(clsdir)):
            idx = os.path.join(clsdir, name, "index.json")
            if not os.path.exists(idx):
                continue
            info = json.load(open(idx))
            images[(info["repo"], info["tag"])] = {"class": cls, "info": info}

    def layer_perf(path):
        q = q2_by_file.get(path)
        r = by_file.get(path)
        if not q or not r:
            return None
        out = {"tar": r["tar"], "orig_comp": r["comp"]}
        for m in ("synth", "b4", "b2", "b3_simenc", "splice"):
            d = q["modes"].get(m)
            if d:
                out[m + "_mbps"] = d.get("mbps")
                out[m + "_out"] = d.get("out")
        return out

    pull = []
    for bw_s in args.bw_mbps.split(","):
        bw = float(bw_s) * 1e6 / 8
        for cache_frac_s in args.cache_hit_frac.split(","):
            cache_frac = float(cache_frac_s)
            per_class = {}
            for (repo, tag), img in images.items():
                layers = []
                for lay in img["info"]["layers"]:
                    if not lay.get("gz"):
                        continue
                    lp = digest_map.get(bare(lay["digest"]))
                    if lp is None:
                        layers.append({"tar": lay["size"] * 2.5,
                                       "orig_comp": lay["size"], "unknown": True})
                        continue
                    p = layer_perf(lp)
                    if p:
                        layers.append(p)
                    else:
                        layers.append({"tar": lay["size"] * 2.5,
                                       "orig_comp": lay["size"], "unknown": True})
                def sys_time(get_rate, get_size):
                    t = 0.0
                    for L in layers:
                        rebuild_s = None
                        rate = get_rate(L)
                        size = get_size(L)
                        if rate and not L.get("unknown"):
                            rebuild_s = L["tar"] / (rate * 1e6)
                        net_s = size / bw if size else 0
                        if cache_frac > 0 and hash(L.get("orig_comp", 0)) % 100 < cache_frac * 100:
                            net_s = 0
                            rebuild_s = 0
                        t += max(rebuild_s or 0, net_s)
                    return t
                row_img = {
                    "class": img["class"], "repo": repo, "tag": tag,
                    "b1_s": sys_time(lambda L: None, lambda L: L.get("orig_comp")),
                    "b4_s": sys_time(lambda L: L.get("b4_mbps"), lambda L: L.get("b4_out", L.get("orig_comp"))),
                    "b2_s": sys_time(lambda L: L.get("b2_mbps"), lambda L: L.get("b2_out", L.get("orig_comp"))),
                    "b3_s": sys_time(lambda L: L.get("b3_simenc_mbps"), lambda L: L.get("b3_simenc_out", L.get("orig_comp"))),
                    "uni_s": sys_time(lambda L: L.get("synth_mbps"), lambda L: L.get("synth_out", L.get("orig_comp"))),
                }
                per_class.setdefault(img["class"], []).append(row_img)
            for cls, rows in per_class.items():
                def med(key):
                    v = sorted(r[key] for r in rows)
                    return v[len(v) // 2] if v else 0
                pull.append({
                    "bw_mbps": float(bw_s), "cache_hit": cache_frac, "class": cls,
                    "n": len(rows),
                    "b1_s": med("b1_s"), "b2_s": med("b2_s"),
                    "b3_s": med("b3_s"), "b4_s": med("b4_s"),
                    "uni_s": med("uni_s"),
                })
                print(f"BW={bw_s}Mbps cache={cache_frac} {cls}: "
                      f"b1={med('b1_s'):.2f}s b2={med('b2_s'):.2f}s "
                      f"b4={med('b4_s'):.2f}s uni={med('uni_s'):.2f}s")

    out = {"ingest": ingest, "pull": pull}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        json.dump(out, f, indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
