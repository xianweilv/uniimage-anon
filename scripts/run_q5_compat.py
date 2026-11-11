#!/usr/bin/env python3

1) diffID : layersynthesis -> zlib decompression + libdeflate  +
    Python  sha256
2) recipe / : recipe_gz / compressionlayersize， recipe
3) : sigma / theta layer
"""
import argparse
import json
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYNTH = os.path.join(ROOT, "src", "tools", "hd_synth")
LD_VERIFY = os.path.join(ROOT, "src", "tools", "ld_verify")
PY_VERIFY = os.path.join(ROOT, "scripts", "pyinflate_verify.py")
OUT = os.path.join(ROOT, "results", "q5_compat.json")


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
    ap.add_argument("--stores", default=os.path.join(ROOT, "data", "stores"))
    ap.add_argument("--m1", default=os.path.join(ROOT, "results", "m1_m3.json"))
    ap.add_argument("--sens-subset", type=int, default=15)
    ap.add_argument("--skip-sens", action="store_true")
    ap.add_argument("--skip-decoders", action="store_true")
    args = ap.parse_args()

    m1 = json.load(open(args.m1))
    recs = m1["policies"]["hybrid"]
    store = os.path.join(args.stores, "store_hybrid")

    ver_results = []
    n_ok_zlib = 0
    n_ld = n_ld_ok = 0
    n_py = n_py_ok = 0
    for i, rec in enumerate(recs):
        if "recipe" not in rec:
            continue
        rsha = rec["recipe"]
        tmp = "/tmp/q5_layer.gz"
        d, err = sh_json([SYNTH, store, "synth", rsha, "--verify", "--out", tmp])
        if not d:
            print(f"synth FAIL {rec['file']}: {err}")
            continue
        ok_zlib = d.get("verify") == "OK"
        n_ok_zlib += ok_zlib
        entry = {"file": rec["file"], "zlib": ok_zlib}
        if not args.skip_decoders:
            r = subprocess.run([LD_VERIFY, tmp, rec["diffid"]],
                               capture_output=True, text=True, timeout=3600)
            n_ld += 1
            ok_ld = "LD_VERIFY_OK" in r.stdout
            n_ld_ok += ok_ld
            entry["libdeflate"] = ok_ld
            if rec.get("tar", 0) < (64 << 20) and n_py < 60:
                r = subprocess.run([sys.executable, PY_VERIFY, tmp, rec["diffid"]],
                                   capture_output=True, text=True, timeout=14400)
                n_py += 1
                ok_py = "PYINFLATE_VERIFY_OK" in r.stdout
                n_py_ok += ok_py
                entry["python"] = ok_py
        ver_results.append(entry)
        if (i + 1) % 20 == 0:
            print(f"  verified {i+1}/{len(recs)}", flush=True)
    if os.path.exists("/tmp/q5_layer.gz"):
        os.remove("/tmp/q5_layer.gz")

    recipe_stats = {
        "n_layers": len(recs),
        "sum_recipe_gz": sum(r.get("recipe_gz", 0) for r in recs),
        "sum_comp": sum(r.get("comp", 0) for r in recs),
        "sum_recipe_raw": sum(r.get("recipe_raw", 0) for r in recs),
    }
    shas = [r["recipe"] for r in recs if "recipe" in r]
    recipe_stats["unique_recipes"] = len(set(shas))
    summ = m1.get("hybrid_summary", {})
    recipe_stats["obj_unique"] = summ.get("obj_unique", 0)
    recipe_stats["obj_comp"] = summ.get("obj_comp_bytes", 0)

    result = {
        "verify": {
            "n_layers": len(ver_results),
            "zlib_ok": n_ok_zlib,
            "libdeflate_tested": n_ld, "libdeflate_ok": n_ld_ok,
            "python_tested": n_py, "python_ok": n_py_ok,
            "details": ver_results,
        },
        "recipe_stats": recipe_stats,
        "sensitivity": {},
    }

    if not args.skip_sens:
        random.seed(7)
        sub = []
        by_cls = {}
        for r in recs:
            if "recipe" not in r:
                continue
            cls = "?"
            for c in ("base", "ai", "app"):
                if ("/" + c + "/") in r["file"].replace("\\", "/"):
                    cls = c
                    break
            by_cls.setdefault(cls, []).append(r)
        for cls, rs in sorted(by_cls.items()):
            small = [r for r in rs if r.get("tar", 0) < 300 * 1024 * 1024]
            random.shuffle(small)
            sub.extend(small[:max(3, args.sens_subset // 3)])
        paths = [os.path.join(ROOT, r["file"]) if not os.path.isabs(r["file"])
                 else r["file"] for r in sub]
        sens = {}
        base_no_dedup = sum(os.path.getsize(p) for p in paths)
        configs = [
            ("sigma0", "hybrid", ["--sigma", "0"]),
            ("sigma256", "hybrid", ["--sigma", "256"]),
            ("sigma1k", "hybrid", []),
            ("sigma4k", "hybrid", ["--sigma", "4096"]),
            ("theta16k", "hybrid", ["--theta", str(16 * 1024)]),
            ("theta32k", "hybrid", ["--theta", str(32 * 1024)]),
            ("theta64k", "hybrid", []),
            ("theta128k", "hybrid", ["--theta", str(128 * 1024)]),
            ("theta256k", "hybrid", ["--theta", str(256 * 1024)]),
        ]
        for label, pol, extra in configs:
            st = os.path.join(args.stores, f"sens_{label}")
            os.system(f"rm -rf {st}")
            print(f"[sens] {label} ...", flush=True)
            all_recs = []
            B = 100
            for i in range(0, len(paths), B):
                r = subprocess.run(
                    [os.path.join(ROOT, "src/tools/hd_ingest"), st, pol] + extra
                    + paths[i:i + B],
                    capture_output=True, text=True, timeout=100000)
                for line in r.stdout.splitlines():
                    if line.strip().startswith("{"):
                        try:
                            all_recs.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass
            if all_recs:
                r = subprocess.run([os.path.join(ROOT, "src/tools/store_stats"), st],
                                   capture_output=True, text=True, timeout=600)
                try:
                    stt = json.loads(r.stdout.strip())
                except Exception:
                    stt = {}
                mbps = []
                for rr in all_recs[:8]:
                    if "recipe" not in rr:
                        continue
                    d, _ = sh_json([SYNTH, st, "synth", rr["recipe"]])
                    if d and d.get("mbps"):
                        mbps.append(d["mbps"])
                mbps.sort()
                sens[label] = {
                    "obj_unique": stt.get("n_obj", 0),
                    "obj_comp": stt.get("obj_comp", 0),
                    "recipe_bytes": stt.get("recipe_bytes", 0),
                    "total_stored": stt.get("obj_comp", 0) + stt.get("recipe_bytes", 0),
                    "ratio_vs_nodedup": (stt.get("obj_comp", 0) + stt.get("recipe_bytes", 0))
                                        / base_no_dedup if base_no_dedup else 0,
                    "synth_mbps_med": mbps[len(mbps) // 2] if mbps else None,
                    "n_new": sum(r.get("new_obj", 0) for r in all_recs),
                    "n_hit": sum(r.get("hit_obj", 0) for r in all_recs),
                }
                print(f"  {label}: objs={sens[label]['obj_unique']} "
                      f"stored={sens[label]['total_stored']/1e6:.1f}MB "
                      f"({sens[label]['ratio_vs_nodedup']*100:.1f}%)", flush=True)
        result["sensitivity"] = sens
        result["sens_base"] = {
            "n_layers": len(paths),
            "no_dedup_bytes": base_no_dedup,
        }

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        json.dump(result, f, indent=0)
    print("saved", OUT)
    print(f"verify: zlib {n_ok_zlib}/{len(ver_results)}  "
          f"libdeflate {n_ld_ok}/{n_ld}  python {n_py_ok}/{n_py}")


if __name__ == "__main__":
    main()
