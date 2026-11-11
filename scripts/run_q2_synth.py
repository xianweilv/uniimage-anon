#!/usr/bin/env python3

eachlayer（hybrid store recipe）eachlayer
  synth  : UniImage Huffman  verify）
  splice : B6 bit-level splicing
  b4     : inflate object + layer gzip L6
  b2     : layer gzip L6（DupHunter rebuild
  inflate:  blob decompression（I/O
B3 (SimEnc ) = fixblock store  synth。
"""
import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYNTH = os.path.join(ROOT, "src", "tools", "hd_synth")
OUT = os.path.join(ROOT, "results", "q2_synth.json")
print_lock = threading.Lock()


def sh(cmd, timeout=14400):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT after %ds (contention or pathological layer)" % timeout
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


def measure_layer(rec, stores, beta):
    f = rec["file"]
    if "recipe" not in rec:
        return None
    rsha = rec["recipe"]
    store = os.path.join(stores, "store_hybrid")
    layer = {
        "file": f, "class": None,
        "orig_comp": rec["comp"], "tar": rec["tar"], "modes": {},
    }
    for cls in ("base", "ai", "app"):
        if f"/{cls}/" in f.replace("\\", "/"):
            layer["class"] = cls
    for mode in ("synth", "splice", "b4", "b2"):
        d, err = sh([SYNTH, store, mode, rsha, "--verify", "--beta", beta])
        if d:
            layer["modes"][mode] = d
        else:
            with print_lock:
                print(f"FAIL {mode} {f}: {err}", flush=True)
    lp = rec["file"] if os.path.isabs(rec["file"]) else os.path.join(ROOT, rec["file"])
    d, err = sh([SYNTH, store, "inflate", lp])
    if d:
        layer["modes"]["inflate"] = d
    with print_lock:
        print(f"[{layer['class']}] {os.path.basename(f)}: "
              f"synth={layer['modes'].get('synth', {}).get('mbps', '?')}MB/s "
              f"verify={layer['modes'].get('synth', {}).get('verify')}", flush=True)
    return layer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stores", default=os.path.join(ROOT, "data", "stores"))
    ap.add_argument("--beta", default="16384")
    ap.add_argument("--jobs", default="10")
    ap.add_argument("--m1", default=os.path.join(ROOT, "results", "m1_m3.json"))
    args = ap.parse_args()

    m1 = json.load(open(args.m1))
    hybrid_recs = m1["policies"]["hybrid"]
    print(f"{len(hybrid_recs)} layers to measure", flush=True)

    with cf.ThreadPoolExecutor(max_workers=int(args.jobs)) as ex:
        futs = [ex.submit(measure_layer, r, args.stores, args.beta)
                for r in hybrid_recs]
        results = []
        for fut in cf.as_completed(futs):
            r = fut.result()
            if r:
                results.append(r)

    fb_recs = m1["policies"].get("fixblock", [])
    by_file = {l["file"]: l for l in results}

    def measure_b3(rec):
        if "recipe" not in rec:
            return None
        match = by_file.get(rec["file"])
        if not match:
            return None
        store = os.path.join(args.stores, "store_fixblock")
        d, err = sh([SYNTH, store, "synth", rec["recipe"], "--verify",
                     "--beta", args.beta])
        if d:
            match["modes"]["b3_simenc"] = d
            return rec["file"]
        return None

    with cf.ThreadPoolExecutor(max_workers=int(args.jobs)) as ex:
        done = [r for r in ex.map(measure_b3, fb_recs) if r]
    print(f"B3 (fixblock synth) measured on {len(done)} layers")

    order = {r["file"]: i for i, r in enumerate(hybrid_recs)}
    results.sort(key=lambda l: order.get(l["file"], 0))
    with open(OUT, "w") as f:
        json.dump(results, f, indent=0)
    print("saved", OUT)

    print("\n=== Q2  MB/s /  blob size / verify）===")
    for cls in ("base", "ai", "app", None):
        sub = [l for l in results if l["class"] == cls]
        if not sub:
            continue
        label = cls or "all"
        line = [label, f"n={len(sub)}"]
        for mode in ("synth", "b3_simenc", "b4", "b2", "splice", "inflate"):
            vals = [l["modes"][mode]["mbps"] for l in sub
                    if mode in l["modes"] and "mbps" in l["modes"][mode]]
            szs = [l["modes"][mode]["out"] / l["orig_comp"] for l in sub
                   if mode in l["modes"] and "out" in l["modes"][mode] and l["orig_comp"]]
            oks = [l["modes"][mode].get("verify") == "OK" for l in sub
                   if mode in l["modes"] and "verify" in l["modes"][mode]]
            if vals:
                vals.sort()
                med = vals[len(vals) // 2]
                rel = (sorted(szs)[len(szs) // 2]) if szs else 0
                vok = all(oks) and len(oks) == len(sub)
                line.append(f"{mode}:{med:.0f}MB/s rel={rel:.3f} "
                            f"{'OK' if vok else 'PARTIAL'}")
        print("  " + "  ".join(line))


if __name__ == "__main__":
    main()
