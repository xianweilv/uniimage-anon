#!/usr/bin/env python3

A)  DupHunter registry（Go, nnzhaocs/DupHunter
   1. crane push  D2 ingestdecompression→filededuplication→storage）
   2. storagedu  ~/duphunter-data）
   3. GET blob latency（cache： re-tar + gzip rebuild
   4. ingestthroughput
B) comparison： B2

/tmp/registry-dup registry  :5000 DUP_DISKCACHE
"""
import argparse
import json
import os
import subprocess
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CRANE = os.path.expanduser("~/bin/crane")
IMAGES = os.path.join(ROOT, "data", "images")
OUT = os.path.join(ROOT, "results", "q_duphunter_real.json")
REG = "localhost:5000"


def sh(cmd, timeout=1800):
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r, time.time() - t0


def du(path):
    r, _ = sh(["du", "-sb", path])
    try:
        return int(r.stdout.split()[0])
    except Exception:
        return 0


def rebuild_oci_tar(img_dir, out_tar, repo_tag):
    idx = json.load(open(os.path.join(img_dir, "index.json")))
    cfg = idx["config"]
    layers = [lay["file"] for lay in idx["layers"]]   # layers/NN.tar.gz
    lines = [{"Config": cfg, "RepoTags": [repo_tag], "Layers": layers}]
    with open(os.path.join(img_dir, "manifest.json"), "w") as f:
        json.dump(lines, f)
    args = ["tar", "cf", out_tar, "-C", img_dir, "manifest.json", cfg] + layers
    r, _ = sh(args)
    return r.returncode == 0


def get_blob_latency(repo, digest, expect_size):
    url = f"http://{REG}/v2/{repo}/blobs/{digest}"
    t0 = time.time()
    try:
        with urllib.request.urlopen(url, timeout=1800) as resp:
            n = 0
            while True:
                chunk = resp.read(1 << 20)
                if not chunk:
                    break
                n += len(chunk)
        dt = time.time() - t0
        return dt, n, n == expect_size
    except Exception as e:
        return -1.0, 0, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--subset", default="memcached__1.6.26,memcached__1.6.27,"
                                        "alpine__3.21.0,alpine__3.21.1,"
                                        "nginx__1.26.0,nginx__1.26.1,"
                                        "python__3.12-slim-bookworm,redis__8.8.0")
    ap.add_argument("--pull-test", type=int, default=4, help="layerlatency")
    args = ap.parse_args()

    results = {"pushed": [], "pull_tests": []}

    # ---- A1: ingest ----
    total_push_s = 0.0
    for name in args.subset.split(","):
        for cls in os.listdir(IMAGES):
            img_dir = os.path.join(IMAGES, cls, name)
            if os.path.isdir(img_dir):
                break
        else:
            print(f"skip {name}: not found")
            continue
        tar_path = f"/tmp/push_{name}.tar"
        if not rebuild_oci_tar(img_dir, tar_path, f"{REG}/{name}:latest"):
            print(f"skip {name}: pack failed")
            continue
        r, dt = sh([CRANE, "push", "--insecure", tar_path,
                    f"{REG}/{name}:latest"])
        ok = r.returncode == 0
        total_push_s += dt
        idx = json.load(open(os.path.join(img_dir, "index.json")))
        comp = sum(lay["size"] for lay in idx["layers"] if lay.get("gz"))
        results["pushed"].append({
            "image": name, "ok": ok, "push_s": dt,
            "orig_comp_bytes": comp, "err": r.stderr[-200:] if not ok else "",
        })
        print(f"[push] {name}: {'ok' if ok else 'FAIL'} {dt:.1f}s "
              f"({comp/1e6:.1f}MB orig)", flush=True)
        os.path.exists(tar_path) and os.remove(tar_path)

    # ---- A2: storage ----
    st_bytes = du(os.path.expanduser("~/duphunter-data"))
    results["storage_bytes"] = st_bytes
    total_comp = sum(p["orig_comp_bytes"] for p in results["pushed"] if p["ok"])
    results["total_orig_comp"] = total_comp
    results["rel_storage"] = st_bytes / total_comp if total_comp else 0
    results["total_push_s"] = total_push_s
    results["push_mbps"] = total_comp / 1e6 / total_push_s if total_push_s else 0
    print(f"storage: {st_bytes/1e9:.3f} GB, orig comp {total_comp/1e9:.3f} GB, "
          f"rel={results['rel_storage']:.3f}")

    import random
    random.seed(11)
    pushed = [p for p in results["pushed"] if p["ok"]]
    test_layers = []
    for p in pushed:
        name = p["image"]
        try:
            with urllib.request.urlopen(
                    f"http://{REG}/v2/{name}/manifests/latest", timeout=60) as m:
                man = json.load(m)
        except Exception:
            continue
        for lay in man.get("layers", []):
            test_layers.append((name, lay["digest"], lay["size"]))
    random.shuffle(test_layers)
    for (repo, digest, size) in test_layers[:args.pull_test]:
        dt, n, ok = get_blob_latency(repo, digest, size)
        mbps = (n / 1e6 / dt) if dt > 0 and n else 0
        results["pull_tests"].append({
            "repo": repo, "digest": digest[:20], "size": size,
            "latency_s": dt, "bytes": n, "match": ok, "mbps": mbps,
        })
        print(f"[pull] {repo} {size/1e6:.1f}MB: {dt:.2f}s "
              f"({mbps:.0f} MB/s) match={ok}", flush=True)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        json.dump(results, f, indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
