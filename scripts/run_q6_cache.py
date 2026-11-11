#!/usr/bin/env python3

: M  Zipf image trace；registry
  -  D（NVMe 500MB/s  S3  320MB/s +  GET 50ms RTT/32
  - synthesis CPU: 32  × layerb4
  -  10Gbps
=50ms）。

comparison frac × storage:
  uni_2lvl  : blob LRU(β) + object LRU(1-β)，β=0.5
  uni_blob   blob（β=1）   uni_obj object（β=0）
  b1        layercache（
  b4_obj    B4 objectcache + inflate+layer gzip（
: layer / object / GBGET / mean,p50,p95,p99
: python3 scripts/run_q6_cache.py [--clients 16] [--per-client 300]
"""
import argparse
import json
import os
import random
import sys
import time
from collections import OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recipe_reader import load_recipe

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "data", "images")
STORE_HY = os.path.join(ROOT, "data", "stores", "store_hybrid")
OUT = os.environ.get("Q6_OUT", os.path.join(ROOT, "results",
                                      "q6_cache.json"))

DELTA = 0.05
GET_RTT = 0.050
GET_STREAMS = 32
BACKEND_BW = 320e6


class LRU:
    def __init__(self, budget, fifo=False):
        self.budget = budget
        self.cur = 0
        self.fifo = fifo
        self.od = OrderedDict()

    def hit(self, key):
        if key in self.od:
            if not self.fifo:
                self.od.move_to_end(key)
            return True
        return False

    def insert(self, key, size):
        if size > self.budget:
            return
        if key in self.od:
            self.od.move_to_end(key)
            return
        self.od[key] = size
        self.cur += size
        while self.cur > self.budget and len(self.od) > 1:
            k, s = self.od.popitem(last=False)
            self.cur -= s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clients", type=int, default=16)
    ap.add_argument("--per-client", type=int, default=300)
    ap.add_argument("--zipf", type=float, default=1.0)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--cores", type=int, default=32)
    ap.add_argument("--egress-gbps", type=float, default=10.0)
    ap.add_argument("--backend-bw-mbps", type=float, default=320.0)
    ap.add_argument("--get-rtt-ms", type=float, default=50.0)
    ap.add_argument("--fracs", default="0.005,0.01,0.02,0.05,0.10,0.20")
    ap.add_argument("--betas", default="0.25,0.5,0.75")
    ap.add_argument("--beta-sweep-frac", type=float, default=0.02)
    ap.add_argument("--m1", default=os.path.join(ROOT, "results", "m1_m3.json"))
    ap.add_argument("--q2", default=os.path.join(ROOT, "results", "q2_synth.json"))
    args = ap.parse_args()

    global GET_RTT, BACKEND_BW
    GET_RTT = args.get_rtt_ms / 1000.0
    BACKEND_BW = args.backend_bw_mbps * 1e6
    m1 = json.load(open(args.m1))
    q2 = json.load(open(args.q2))

    def normp(f):
        f = f.replace("\\", "/")
        return os.path.normpath(f if os.path.isabs(f) else os.path.join(ROOT, f))

    q2_by = {normp(l["file"]): l for l in q2}
    rec_by_sha = {}
    for r in m1["policies"]["hybrid"]:
        if "recipe" in r and r.get("blob_sha"):
            rec_by_sha[r["blob_sha"]] = r

    images = {}
    layer_info = {}
    for cls in os.listdir(IMAGES):
        clsdir = os.path.join(IMAGES, cls)
        if not os.path.isdir(clsdir):
            continue
        for name in sorted(os.listdir(clsdir)):
            ip = os.path.join(clsdir, name, "index.json")
            if not os.path.exists(ip):
                continue
            info = json.load(open(ip))
            digs = []
            for lay in info["layers"]:
                if not lay.get("gz"):
                    continue
                dg = lay["digest"].split(":")[-1].split(".tar.gz")[0].split(".")[0]
                digs.append(dg)
                layer_info[dg] = lay["size"]
            images[(info["repo"], info["tag"])] = {"class": cls, "layers": digs}

    obj_size_map = {}
    _meta = os.path.join(STORE_HY, "objmeta.jsonl")
    if os.path.exists(_meta):
        with open(_meta) as f:
            for line in f:
                try:
                    j = json.loads(line)
                    obj_size_map[j["sha"]] = j["comp"]
                except Exception:
                    pass
    perf = {}     # dg -> {"synth": (mbps, out), "b4": (mbps, out), "b1_out", "tar"}
    obj_info = {}  # dg -> [(sha, size)]
    for dg in layer_info:
        rec = rec_by_sha.get(dg)
        if not rec:
            continue
        ql = q2_by.get(normp(rec["file"]))
        if not ql:
            continue
        p = {"b1_out": rec["comp"], "tar": rec["tar"]}
        for m in ("synth", "b4"):
            d = ql["modes"].get(m)
            if d:
                p[m] = (d.get("mbps"), d.get("out", rec["comp"]))
        try:
            rp = load_recipe(STORE_HY, rec["recipe"])
        except Exception:
            continue
        objs = []
        for (sha, raw_len, crc) in rp.objs:
            objs.append((sha, obj_size_map.get(sha, 0)))
        perf[dg] = p
        obj_info[dg] = objs
    img_list = []
    for key, img in images.items():
        if all(dg in perf for dg in img["layers"]):
            img_list.append(key)

    random.seed(args.seed)
    cls_rank = {"base": 0, "app": 1, "ai": 2}
    img_list.sort(key=lambda k: (cls_rank.get(images[k]["class"], 3),
                                 random.random()))
    n = len(img_list)
    zipf_w = [1.0 / (i + 1) ** args.zipf for i in range(n)]
    tot_w = sum(zipf_w)
    img_pos = {k: i for i, k in enumerate(img_list)}

    def sample_img(rnd):
        x = rnd.random() * tot_w
        acc = 0
        for i, w in enumerate(zipf_w):
            acc += w
            if acc >= x:
                if os.environ.get("Q6_DEBUG"):
                    dbg_n[0] += 1
                    if dbg_n[0] <= 8:
                        print(f"DBG samp x={x:.3f} i={i} tot_w={tot_w:.3f} "
                              f"cls={images[img_list[i]]['class']}")
                return img_list[i]
        return img_list[-1]

    dbg_n = [0]

    traces = []
    for c in range(args.clients):
        rnd = random.Random(args.seed * 1000 + c)
        traces.append([sample_img(rnd) for _ in range(args.per_client)])
    if os.environ.get("Q6_DEBUG"):
        from collections import Counter
        print("DBG img_list:", dict(Counter(images[k]["class"] for k in img_list)))
        print("DBG first10 ranks:",
              [img_pos[k] for tr_ in traces[:1] for k in tr_[:10]])
        print("DBG traces:",
              dict(Counter(images[k]["class"] for tr_ in traces for k in tr_)))

    stored = {
        "b1": m1["layer_dedup_bytes"],
        "b4": m1["hybrid_summary"]["obj_comp_bytes"] + m1["hybrid_summary"]["recipe_bytes"],
        "uni": m1["hybrid_summary"]["obj_comp_bytes"] + m1["hybrid_summary"]["recipe_bytes"],
    }
    EGRESS = args.egress_gbps * 1e9 / 8

    def simulate(sysname, budget, beta, backend, fifo=False, batch_mb=0.0):
        if backend == "nvme":
            d_bw, get_extra = 500e6, 0.0
        else:
            d_bw = BACKEND_BW
            get_extra = GET_RTT / GET_STREAMS
        if sysname == "b1":
            blob_c, obj_c = LRU(budget, fifo), None
        elif sysname == "b4":
            blob_c, obj_c = None, LRU(budget, fifo)
        else:
            blob_c = LRU(beta * budget, fifo) if beta > 0 else None
            obj_c = LRU((1 - beta) * budget, fifo) if beta < 1.0 else None

        stats = {"lhit": 0, "ltot": 0, "ohit_b": 0, "oneed_b": 0,
                 "read_b": 0, "gets": 0}
        active = []            # dict: net/cpu/disk need+done, img, client, t0
        pending = {}           # (client, req_i) -> [layers_left, t0]
        next_i = [0] * args.clients
        lat = []
        t = 0.0

        def start_image(c):
            key = traces[c][next_i[c]]
            t0 = t
            lays = images[key]["layers"]
            pending[(c, next_i[c])] = [len(lays), t0]
            for dg in lays:
                p = perf[dg]
                stats["ltot"] += 1
                net = cpu = disk = 0.0
                out = p["b1_out"]
                if sysname == "b1":
                    out = p["b1_out"]
                    miss_b = 0
                    if blob_c.hit(dg):
                        stats["lhit"] += 1
                    else:
                        blob_c.insert(dg, out)
                        miss_b = out
                        stats["read_b"] += out
                        stats["gets"] += 1
                    active.append({"net": out / EGRESS, "cpu": 0.0,
                                   "bw": miss_b / d_bw,
                                   "rtt": get_extra if miss_b else 0.0,
                                   "c": c, "i": next_i[c]})
                    continue
                syskey = "synth" if sysname == "uni" else "b4"
                out = p.get(syskey, (None, out))[1] or out
                if blob_c is not None and blob_c.hit(dg):
                    blob_c.insert(dg, out)
                    stats["lhit"] += 1
                    active.append({"net": out / EGRESS, "cpu": 0.0,
                                   "bw": 0.0, "rtt": 0.0,
                                   "c": c, "i": next_i[c]})
                    continue
                if blob_c is not None:
                    blob_c.insert(dg, out)
                objs = obj_info[dg]
                miss_b = miss_n = 0
                for (sha, sz) in objs:
                    stats["oneed_b"] += sz
                    if obj_c is not None and obj_c.hit(sha):
                        stats["ohit_b"] += sz
                    else:
                        miss_b += sz
                        miss_n += 1
                if obj_c is not None:
                    for (sha, sz) in objs:
                        obj_c.insert(sha, sz)
                if batch_mb > 0 and miss_b > 0:
                    miss_n = -(-miss_b // int(batch_mb * 1e6))
                stats["read_b"] += miss_b
                stats["gets"] += miss_n
                rate = p.get(syskey, (None,))[0]
                cpu = p["tar"] / (rate * 1e6) if rate else 0.0
                active.append({"net": out / EGRESS, "cpu": cpu,
                               "bw": miss_b / d_bw, "rtt": miss_n * get_extra,
                               "c": c, "i": next_i[c]})
            next_i[c] += 1

        steps = 0
        while True:
            for c in range(args.clients):
                if (c, next_i[c] - 1) not in pending and next_i[c] < args.per_client:
                    start_image(c)
            if not active and all(next_i[c] >= args.per_client
                                  for c in range(args.clients)):
                break
            n_net = sum(1 for r in active if r["net"] > 0)
            n_cpu = sum(1 for r in active if r["cpu"] > 0)
            n_bw = sum(1 for r in active if r["bw"] > 0)
            done = []
            for r in active:
                if r["net"] > 0:
                    r["net"] = max(0.0, r["net"] - DELTA / max(1, n_net))
                if r["cpu"] > 0:
                    r["cpu"] = max(0.0,
                                   r["cpu"] - DELTA * args.cores / max(1, n_cpu))
                if r["bw"] > 0:
                    r["bw"] = max(0.0, r["bw"] - DELTA / max(1, n_bw))
                if r["rtt"] > 0:
                    r["rtt"] = max(0.0, r["rtt"] - DELTA)
                if (r["net"] == 0 and r["cpu"] == 0 and r["bw"] == 0
                        and r["rtt"] == 0):
                    done.append(r)
            for r in done:
                active.remove(r)
                key = (r["c"], r["i"])
                pending[key][0] -= 1
                if pending[key][0] == 0:
                    lat.append(t - pending[key][1])
                    del pending[key]
            t += DELTA
            steps += 1
            if steps > 4_000_000:
                print("SIM TIMEOUT", flush=True)
                break
        lat.sort()
        return {
            "layer_hit": stats["lhit"] / max(1, stats["ltot"]),
            "obj_hit_bytes": stats["ohit_b"] / max(1, stats["oneed_b"]),
            "fifo": fifo,
            "backend_read_gb": stats["read_b"] / 1e9,
            "backend_gets": stats["gets"],
            "mean_s": sum(lat) / max(1, len(lat)),
            "p50_s": lat[min(len(lat) - 1, len(lat) // 2)],
            "p95_s": lat[min(len(lat) - 1, int(len(lat) * 0.95))],
            "p99_s": lat[min(len(lat) - 1, int(len(lat) * 0.99))],
            "n_img": len(lat),
            "sim_time_s": t,
        }

    results = {"config": {
        "clients": args.clients, "per_client": args.per_client,
        "zipf": args.zipf, "seed": args.seed, "cores": args.cores,
        "egress_gbps": args.egress_gbps, "delta_s": DELTA,
        "get_rtt_s": GET_RTT, "get_streams": GET_STREAMS,
        "n_images": len(img_list), "stored_bytes": stored,
    }, "size_sweep": [], "beta_sweep": [], "policy": []}

    print(f"images with full perf: {len(img_list)}", flush=True)
    print("=== 1) S3 : 320MB/s  + GET RTT 50ms/32===", flush=True)
    for frac_s in args.fracs.split(","):
        frac = float(frac_s)
        for sysname in ("uni_2lvl", "uni_2lvl_batch", "uni_blob", "uni_obj",
                        "b1", "b4_obj"):
            base = "uni" if sysname.startswith("uni") else \
                ("b1" if sysname == "b1" else "b4")
            beta = {"uni_2lvl": 0.5, "uni_2lvl_batch": 0.5, "uni_blob": 1.0,
                    "uni_obj": 0.0, "b1": 1.0, "b4_obj": 0.0}[sysname]
            t0 = time.time()
            row = {"frac": frac, "sys": sysname, "backend": "s3"}
            row.update(simulate(base, frac * stored[base], beta, "s3",
                                batch_mb=(16.0 if sysname.endswith("_batch")
                                          else 0.0)))
            results["size_sweep"].append(row)
            print(f"frac={frac:.1%} {sysname:8s} Lhit={row['layer_hit']*100:5.1f}% "
                  f"Ohit={row['obj_hit_bytes']*100:5.1f}% "
                  f"read={row['backend_read_gb']:7.2f}GB gets={row['backend_gets']:9d} "
                  f"mean={row['mean_s']:7.2f}s p99={row['p99_s']:7.2f}s "
                  f"({time.time()-t0:.0f}s)", flush=True)

    print("=== 2) NVMe 500MB/s,  GET RTT, frac=2%）===", flush=True)
    for sysname in ("uni_2lvl", "uni_blob", "uni_obj", "b1", "b4_obj"):
        base = "uni" if sysname.startswith("uni") else \
            ("b1" if sysname == "b1" else "b4")
        beta = {"uni_2lvl": 0.5, "uni_blob": 1.0, "uni_obj": 0.0,
                "b1": 1.0, "b4_obj": 0.0}[sysname]
        row = {"frac": 0.02, "sys": sysname, "backend": "nvme"}
        row.update(simulate(base, 0.02 * stored[base], beta, "nvme"))
        results["size_sweep"].append(row)
        print(f"nvme 2% {sysname:8s} Lhit={row['layer_hit']*100:5.1f}% "
              f"mean={row['mean_s']:7.2f}s p99={row['p99_s']:7.2f}s", flush=True)

    print("=== 3) β S3, frac=2%）===", flush=True)
    for beta_s in args.betas.split(","):
        beta = float(beta_s)
        row = {"frac": args.beta_sweep_frac, "beta": beta, "backend": "s3"}
        row.update(simulate("uni", args.beta_sweep_frac * stored["uni"],
                            beta, "s3"))
        results["beta_sweep"].append(row)
        print(f"beta={beta:.2f} Lhit={row['layer_hit']*100:5.1f}% "
              f"Ohit={row['obj_hit_bytes']*100:5.1f}% "
              f"read={row['backend_read_gb']:7.2f}GB "
              f"mean={row['mean_s']:7.2f}s p99={row['p99_s']:7.2f}s", flush=True)

    print("=== 4) policy LRU vs FIFO（S3, frac=2%, beta=0.5）===", flush=True)
    results["policy"] = []
    for pol in ("lru", "fifo"):
        row = {"policy": pol, "frac": 0.02, "beta": 0.5, "backend": "s3"}
        row.update(simulate("uni", 0.02 * stored["uni"], 0.5, "s3",
                            fifo=(pol == "fifo")))
        results["policy"].append(row)
        print(f"{pol:4s} Lhit={row['layer_hit']*100:5.1f}% "
              f"Ohit={row['obj_hit_bytes']*100:5.1f}% "
              f"read={row['backend_read_gb']:7.2f}GB "
              f"mean={row['mean_s']:7.2f}s p99={row['p99_s']:7.2f}s", flush=True)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(results, open(OUT, "w"), indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
