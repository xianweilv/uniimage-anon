#!/usr/bin/env python3

32 layersynthesis/layerregistry
image）：

  1) rebuild: N ∈ {1,2,4,8,16}  worker layer
     - synth (UniImage Huffman synthesis)  vs  b4 (inflate+layer gzip L6)
     - : throughput MB/s、 T_1/T_N、layerlatency mean/p99、verify
  2) B1 : N ∈ {1,4,8}  blob（dd iflag=direct cache）
  3) ingest: N ∈ {1,2,4,8}  worker imageingest store（
     hd_ingest，objectdeduplication MB/s
: python3 scripts/run_q8_concurrency.py [--max-workers 16]
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "data", "images")
STORE_HY = os.path.join(ROOT, "data", "stores", "store_hybrid")
SYNTH = os.path.join(ROOT, "src", "tools", "hd_synth")
INGEST = os.path.join(ROOT, "src", "tools", "hd_ingest")
M1 = os.path.join(ROOT, "results", "m1_m3.json")
OUT = os.path.join(ROOT, "results", "q8_concurrency.json")


def sh_json(cmd, timeout=14400):
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
    ap.add_argument("--workers", default="1,2,4,8,16")
    ap.add_argument("--per-class", type=int, default=15)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    m1 = json.load(open(M1))
    recs = [r for r in m1["policies"]["hybrid"]
            if "recipe" in r and r.get("blob_sha")]
    by_cls = {}
    for r in recs:
        cls = None
        for c in ("base", "ai", "app"):
            if f"/{c}/" in r["file"].replace("\\", "/"):
                cls = c
                break
        if cls:
            by_cls.setdefault(cls, []).append(r)
    subset = []
    for cls, rs in by_cls.items():
        rs.sort(key=lambda r: r["tar"])
        k = max(1, len(rs) // args.per_class)
        subset.extend(rs[::k][:args.per_class])
    tot_tar = sum(r["tar"] for r in subset)
    tot_comp = sum(r["comp"] for r in subset)
    print(f"subset={len(subset)} layers tar={tot_tar/1e9:.2f}GB "
          f"comp={tot_comp/1e9:.2f}GB", flush=True)

    results = {"config": {
        "subset_layers": len(subset), "tar_bytes": tot_tar,
        "comp_bytes": tot_comp, "nproc": os.cpu_count(),
    }, "pull_scaling": [], "b1_read": [], "ingest_scaling": []}

    def lane_work(lane_layers, mode, lat_out, verify_out):
        for r in lane_layers:
            t0 = time.time()
            d, err = sh_json([SYNTH, STORE_HY, mode, r["recipe"], "--verify"])
            dt = time.time() - t0
            lat_out.append((r["tar"], dt))
            verify_out.append(bool(d) and d.get("verify") == "OK")

    base_wall = {}
    for mode in ("synth", "b4"):
        for N in [int(x) for x in args.workers.split(",")]:
            lanes = [subset[i::N] for i in range(N)]
            lats = [[] for _ in range(N)]
            vers = [[] for _ in range(N)]
            ths = []
            t0 = time.time()
            for i in range(N):
                th = threading.Thread(target=lane_work,
                                      args=(lanes[i], mode, lats[i], vers[i]))
                th.start()
                ths.append(th)
            for th in ths:
                th.join()
            wall = time.time() - t0
            all_lat = [x for ls in lats for x in ls]
            all_ok = all(x for vs in vers for x in vs) if all_lat else False
            if N == 1:
                base_wall[mode] = wall
            row = {
                "mode": mode, "workers": N, "wall_s": wall,
                "agg_mbps": tot_tar / wall / 1e6,
                "speedup": base_wall[mode] / wall,
                "per_layer_p50_s": statistics.median([d for _, d in all_lat]),
                "per_layer_p99_s": sorted(d for _, d in all_lat)[
                    max(0, int(len(all_lat) * 0.99) - 1)],
                "all_verify_ok": all_ok,
            }
            results["pull_scaling"].append(row)
            print(f"{mode:5s} N={N:2d} wall={wall:7.2f}s agg={row['agg_mbps']:7.1f}MB/s "
                  f"speedup={row['speedup']:.2f}x p50={row['per_layer_p50_s']:.2f}s "
                  f"p99={row['per_layer_p99_s']:.2f}s verify={'OK' if all_ok else 'FAIL'}",
                  flush=True)

    def read_lane(lane_files, out_mb):
        tot = 0
        for f in lane_files:
            r = subprocess.run(
                ["dd", f"if={f}", "of=/dev/null", "bs=1M", "iflag=direct"],
                capture_output=True)
            tot += os.path.getsize(f)
        out_mb.append(tot)

    b1_files = []
    for r in subset:
        f = r["file"]
        if not os.path.isabs(f):
            f = os.path.join(ROOT, f)
        b1_files.append(f)
    for N in (1, 4, 8):
        lanes = [b1_files[i::N] for i in range(N)]
        outs = [[] for _ in range(N)]
        ths = []
        t0 = time.time()
        for i in range(N):
            th = threading.Thread(target=read_lane, args=(lanes[i], outs[i]))
            th.start()
            ths.append(th)
        for th in ths:
            th.join()
        wall = time.time() - t0
        row = {"workers": N, "wall_s": wall,
               "agg_mbps": tot_comp / wall / 1e6}
        results["b1_read"].append(row)
        print(f"b1read N={N} wall={wall:.2f}s agg={row['agg_mbps']:.1f}MB/s",
              flush=True)

    ingest_imgs = []
    for cls in ("base", "app"):
        clsdir = os.path.join(IMAGES, cls)
        if not os.path.isdir(clsdir):
            continue
        for name in sorted(os.listdir(clsdir)):
            ip = os.path.join(clsdir, name, "index.json")
            if not os.path.exists(ip):
                continue
            info = json.load(open(ip))
            gzs = [os.path.join(clsdir, name, l["file"])
                   for l in info["layers"] if l.get("gz")]
            if gzs:
                ingest_imgs.append((info["repo"], info["tag"], gzs))
    ingest_imgs = ingest_imgs[:32]
    tot_in = sum(os.path.getsize(g) for _, _, gzs in ingest_imgs for g in gzs)
    print(f"ingest images={len(ingest_imgs)} comp={tot_in/1e9:.2f}GB", flush=True)

    def ingest_lane(store, img_lists, ok_out):
        for gzs in img_lists:
            for g in gzs:
                d, err = sh_json([INGEST, store, "hybrid", g])
                ok_out.append(bool(d) and d.get("error", 0) == 0)

    for N in (1, 2, 4, 8):
        store = os.path.join(ROOT, "data", "stores", f"scratch_ingest_{N}")
        subprocess.run(["rm", "-rf", store])
        work = [gzs for _, _, gzs in ingest_imgs]
        lanes = [work[i::N] for i in range(N)]
        oks = [[] for _ in range(N)]
        ths = []
        t0 = time.time()
        for i in range(N):
            th = threading.Thread(target=ingest_lane,
                                  args=(store, lanes[i], oks[i]))
            th.start()
            ths.append(th)
        for th in ths:
            th.join()
        wall = time.time() - t0
        all_ok = all(x for vs in oks for x in vs) if oks else False
        row = {"workers": N, "wall_s": wall,
               "agg_mbps": tot_in / wall / 1e6,
               "all_ok": all_ok, "store_bytes": 0}
        du = subprocess.run(["du", "-sb", store], capture_output=True, text=True)
        try:
            row["store_bytes"] = int(du.stdout.split()[0])
        except Exception:
            pass
        subprocess.run(["rm", "-rf", store])
        results["ingest_scaling"].append(row)
        print(f"ingest N={N} wall={wall:.1f}s agg={row['agg_mbps']:.1f}MB/s "
              f"ok={all_ok}", flush=True)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(results, open(OUT, "w"), indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
