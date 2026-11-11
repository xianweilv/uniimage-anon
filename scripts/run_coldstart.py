#!/usr/bin/env python3

1.  HTTP objectstorage），configurationlatency RTT
2. image：
   a.  manifest/config（2  HTTP
   b.
      - NAT: imagelayer（
      - eStargz : filecompression
      - UniImage : filecompression
   c.  tmpfs（layer）
3. measurefork + chroot + exec

 HTTP file
"""
import http.server
import json
import os
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "data", "rootfs_cache")
IDX = os.path.join(ROOT, "data", "layer_index.json")
OUT = os.path.join(ROOT, "results", "coldstart.json")

RTT_MS = 5
BW_MBPS = 200
PORT = 18923



class DelayedHandler(http.server.BaseHTTPRequestHandler):

    def do_GET(self):
        path = self.path.lstrip("/")
        if not os.path.isfile(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        time.sleep(RTT_MS / 1000.0)
        transfer_time = size / (BW_MBPS * 1e6 / 8)
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.end_headers()
        chunk_size = int(BW_MBPS * 1e6 / 8 * transfer_time / 20) or size
        chunk_size = max(chunk_size, 4096)
        sent = 0
        with open(path, "rb") as f:
            while sent < size:
                data = f.read(min(chunk_size, size - sent))
                if not data:
                    break
                self.wfile.write(data)
                sent += len(data)
                if sent < size:
                    time.sleep(transfer_time / 20)

    def do_HEAD(self):
        path = self.path.lstrip("/")
        if not os.path.isfile(path):
            self.send_error(404)
            return
        time.sleep(RTT_MS / 1000.0)
        self.send_response(200)
        self.send_header("Content-Length", str(os.path.getsize(path)))
        self.end_headers()

    def log_message(self, *args):
        pass


def start_server(tmpdir):
    os.makedirs(tmpdir, exist_ok=True)
    os.chdir(tmpdir)
    socketserver.TCPServer.allow_reuse_address = True
    server = socketserver.TCPServer(("127.0.0.1", PORT), DelayedHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server, f"http://127.0.0.1:{PORT}"


# ============ measure ============

def http_get(url, timeout=30):
    t0 = time.perf_counter()
    with urllib.request.urlopen(url, timeout=timeout) as r:
        data = r.read()
    return data, time.perf_counter() - t0


def fetch_and_write(url, local_path):
    data, t = http_get(url)
    with open(local_path, "wb") as f:
        f.write(data)
    return t


def measure_image(image_name, cls, base_url, workdir):
    rootfs = os.path.join(CACHE, image_name)
    if not os.path.isdir(rootfs):
        return None

    q4r_path = os.path.join(ROOT, "results", "q4_range.json")
    if not os.path.exists(q4r_path):
        return None
    q4r = json.load(open(q4r_path))
    rr = next((r for r in q4r["rows"] if r["image"] == image_name), None)
    if rr is None:
        return None

    from run_q4_modes import boot_file_set
    idx = json.load(open(IDX))
    img_info = next((l for l in idx["layers"] if l["image"] == image_name.rsplit("__", 1)[1]
                     and l["class"] == cls), None)

    images_dir = os.path.join(ROOT, "data", "images")
    entry = []
    img_dir_name = image_name.split("__", 1)[1] if "__" in image_name else image_name
    for d_name in os.listdir(os.path.join(images_dir, cls)):
        if img_dir_name.startswith(d_name.rsplit("__", 1)[0]):
            d = os.path.join(images_dir, cls, d_name)
            for f in os.listdir(d):
                if f.startswith("sha256:"):
                    try:
                        cfg = json.load(open(os.path.join(d, f)))
                        entry = cfg.get("config", {}).get("Entrypoint") or \
                                cfg.get("config", {}).get("Cmd") or []
                    except Exception:
                        pass
                    break
            break

    boot = boot_file_set(rootfs, entry)
    if not boot:
        return None

    import zlib

    tmpdir = os.path.join(workdir, "serve")
    os.makedirs(tmpdir, exist_ok=True)

    esg_files = {}
    for bf in sorted(boot):
        p = os.path.join(rootfs, bf)
        try:
            raw = open(p, "rb").read()
        except OSError:
            continue
        co = zlib.compressobj(6, zlib.DEFLATED, 16 + 15)
        comp = co.compress(raw) + co.flush()
        fn = bf.replace("/", "_")
        fp = os.path.join(tmpdir, fn)
        with open(fp, "wb") as f:
            f.write(comp)
        esg_files[bf] = (fn, len(comp), len(raw))


    total_bytes = 0
    if img_info:
        for l in idx["layers"]:
            if l["image"] == image_name.rsplit("__", 1)[1].split("__")[-1] \
               and l["class"] == cls:
                total_bytes += l["size"]

    # manifest + config
    manifest = json.dumps({"boot_files": sorted(boot), "n": len(boot)}).encode()
    with open(os.path.join(tmpdir, "manifest"), "wb") as f:
        f.write(manifest)
    with open(os.path.join(tmpdir, "config"), "wb") as f:
        f.write(b"{}" * 100)

    results = {"image": image_name, "class": cls, "n_boot": len(esg_files)}

    outdir = os.path.join(workdir, "esg_out")
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir)
    t0 = time.perf_counter()
    # manifest + config
    http_get(f"{base_url}/manifest")
    http_get(f"{base_url}/config")
    for bf, (fn, clen, rlen) in esg_files.items():
        data, _ = http_get(f"{base_url}/{fn}")
        with open(os.path.join(outdir, fn), "wb") as f:
            f.write(data)
    t_esg = time.perf_counter() - t0
    results["t_esg"] = round(t_esg, 4)

    outdir = os.path.join(workdir, "uni_out")
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir)
    t0 = time.perf_counter()
    http_get(f"{base_url}/manifest")
    http_get(f"{base_url}/config")
    for bf, (fn, clen, rlen) in esg_files.items():
        data, _ = http_get(f"{base_url}/{fn}")
        with open(os.path.join(outdir, fn), "wb") as f:
            f.write(data)
    t_uni = time.perf_counter() - t0
    results["t_uni_sub"] = round(t_uni, 4)

    if total_bytes > 0 and img_info:
        outdir = os.path.join(workdir, "nat_out")
        shutil.rmtree(outdir, ignore_errors=True)
        os.makedirs(outdir)
        layer_files = []
        for l in idx["layers"]:
            if l["class"] == cls and l["image"] == image_name.rsplit("__", 1)[1].split("__")[-1]:
                src = os.path.join(ROOT, l["path"])
                if os.path.exists(src):
                    dst = os.path.join(tmpdir, f"layer_{len(layer_files)}")
                    if not os.path.exists(dst):
                        os.link(src, dst)
                    layer_files.append(f"layer_{len(layer_files)}")
        t0 = time.perf_counter()
        http_get(f"{base_url}/manifest")
        http_get(f"{base_url}/config")
        for lf in layer_files:
            data, _ = http_get(f"{base_url}/{lf}")
        t_nat = time.perf_counter() - t0
        results["t_nat"] = round(t_nat, 4)
        results["nat_bytes"] = total_bytes
        for lf in layer_files:
            p = os.path.join(tmpdir, lf)
            if os.path.exists(p):
                os.unlink(p)

    exe = None
    if entry:
        exe_name = entry[0].split("/")[-1]
        for d in ("bin", "sbin", "usr/bin", "usr/sbin"):
            p = os.path.join(rootfs, d, exe_name)
            if os.path.exists(p):
                exe = p
                break
    if exe and os.path.exists(exe):
        t0 = time.perf_counter()
        try:
            r = subprocess.run(["timeout", "0.1", "chroot", rootfs, exe, "--version"],
                               capture_output=True, timeout=0.5)
            t_exec = time.perf_counter() - t0
            results["t_exec"] = round(t_exec, 4)
        except Exception:
            results["t_exec"] = -1
    else:
        results["t_exec"] = -1

    return results


def main():
    os.makedirs(OUT.rsplit("/", 1)[0], exist_ok=True)
    idx = json.load(open(IDX))
    images = {}
    for l in idx["layers"]:
        key = "%s__%s__%s" % (l["class"], l["repo"].replace("/", "_"), l["tag"])
        images[key] = l["class"]

    import random
    random.seed(42)
    from collections import defaultdict
    by_cls = defaultdict(list)
    for name, cls in images.items():
        rootfs = os.path.join(CACHE, name)
        if os.path.isdir(os.path.join(rootfs, "bin")) or \
           os.path.isdir(os.path.join(rootfs, "usr")):
            by_cls[cls].append(name)

    sample = []
    for cls in ["base", "ai", "app"]:
        pool = sorted(by_cls[cls])
        random.shuffle(pool)
        sample.extend([(n, cls) for n in pool[:5]])

    print(f"Testing {len(sample)} images...")

    all_results = []
    with tempfile.TemporaryDirectory(prefix="coldstart_") as workdir:
        server, base_url = start_server(os.path.join(workdir, "serve"))
        try:
            for name, cls in sample:
                print(f"  {cls}/{name}...", end="", flush=True)
                r = measure_image(name, cls, base_url, workdir)
                if r:
                    all_results.append(r)
                    print(f" esg={r['t_esg']*1000:.0f}ms uni={r['t_uni_sub']*1000:.0f}ms"
                          f" nat={r.get('t_nat', 0)*1000:.0f}ms exec={r.get('t_exec', -1)*1000:.0f}ms")
                else:
                    print(" SKIP")
        finally:
            server.shutdown()

    json.dump({"results": all_results, "rtt_ms": RTT_MS, "bw_mbps": BW_MBPS,
               "n_sample": len(sample)},
              open(OUT, "w"), indent=0)

    import statistics as st
    by = defaultdict(list)
    for r in all_results:
        by[r["class"]].append(r)
    print("\n=== Per-class medians (real HTTP cold start, ms) ===")
    for cls in ["base", "ai", "app"]:
        rs = by[cls]
        if not rs:
            continue
        m = lambda k: st.median([r[k] for r in rs if r.get(k, 0) > 0]) * 1000 if any(r.get(k, 0) > 0 for r in rs) else 0
        e = st.median([r["t_exec"] for r in rs if r.get("t_exec", -1) > 0]) * 1000 \
            if any(r.get("t_exec", -1) > 0 for r in rs) else 0
        print(f"{cls:5s}  eStargz={m('t_esg'):.0f}ms  UniImage={m('t_uni_sub'):.0f}ms  "
              f"NAT={m('t_nat'):.0f}ms  exec_overhead={e:.0f}ms")


if __name__ == "__main__":
    main()
