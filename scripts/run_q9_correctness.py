#!/usr/bin/env python3

A) incremental store）:
    repo  tag  (t1 -> t2):
     -  store  t1  recipe+object（
     - incremental t2: object（= t2 layer
       hd_synth synth --verify（zlib decompression + diffID
   :  t2 layer diffID OK
B) verification:
   - rebuildimage rootfs（whiteout + ENTRYPOINT ELF  => boot
   -  boot file:  recipe Meta/Pad/Ref）object raw deflate
      tar  tar.gz
   - :  512B tar  linkname；directory/:  magic
   : file; image boot object
"""
import argparse
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import zlib
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recipe_reader import load_recipe, extract_rootfs, SEG_META, SEG_PAD, SEG_REF
import run_q4_modes as q4mod

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "data", "images")
STORE_HY = os.path.join(ROOT, "data", "stores", "store_hybrid")
SYNTH = os.path.join(ROOT, "src", "tools", "hd_synth")
M1 = os.path.join(ROOT, "results", "m1_m3.json")
OUT = os.path.join(ROOT, "results", "q9_correctness.json")

print_lock = threading.Lock()


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


def norm_name(nm):
    while nm.startswith("./"):
        nm = nm[2:]
    return nm.lstrip("/")


def tag_key(t):
    nums = [int(x) if x.isdigit() else x
            for x in re.findall(r"\d+|\D+", t)]
    return ([(0, x) if isinstance(x, int) else (1, x) for x in nums], t)


def link_into(client, store, kind, sha):
    dst = os.path.join(client, kind, sha[:2], sha)
    if os.path.exists(dst):
        return 0
    src = os.path.join(store, kind, sha[:2], sha)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy(src, dst)
    return os.path.getsize(dst)


# ---------------- A) incremental ----------------
def run_incremental(rec_by_sha, images_by_repo):
    pairs = []
    for (cls, repo), tags in sorted(images_by_repo.items()):
        tags.sort(key=lambda t: tag_key(t[0]))
        for a, b in zip(tags, tags[1:]):
            pairs.append((cls, repo, a, b))

    def one_pair(idx_pair):
        i, (cls, repo, t1, t2) = idx_pair
        client = tempfile.mkdtemp(prefix=f"cli_{i}_", dir="/tmp")
        try:
            for lay in t1[1]["info"]["layers"]:
                dg = lay["digest"].split(":")[-1].split(".")[0]
                r = rec_by_sha.get(dg)
                if not r:
                    return None
                rec = load_recipe(STORE_HY, r["recipe"])
                link_into(client, STORE_HY, "recipes", r["recipe"])
                for (sha, *_ ) in rec.objs:
                    link_into(client, STORE_HY, "objects", sha)
            inc_bytes = 0
            std_bytes = 0
            n_new_obj = 0
            n_shared_obj = 0
            verifies = []
            for lay in t2[1]["info"]["layers"]:
                dg = lay["digest"].split(":")[-1].split(".")[0]
                r = rec_by_sha.get(dg)
                if not r:
                    return None
                std_bytes += lay["size"]
                rec = load_recipe(STORE_HY, r["recipe"])
                inc_bytes += link_into(client, STORE_HY, "recipes", r["recipe"])
                for (sha, *_ ) in rec.objs:
                    got = link_into(client, STORE_HY, "objects", sha)
                    if got:
                        inc_bytes += got
                        n_new_obj += 1
                    else:
                        n_shared_obj += 1
                d, err = sh_json([SYNTH, client, "synth", r["recipe"], "--verify"])
                ok = bool(d) and d.get("verify") == "OK"
                verifies.append(ok)
                if not ok:
                    with print_lock:
                        print(f"INCR FAIL {repo}:{t2[0]} {dg[:12]} {err}",
                              flush=True)
            return {
                "class": cls, "repo": repo, "from": t1[0], "to": t2[0],
                "layers": len(verifies), "all_ok": all(verifies),
                "std_bytes": std_bytes, "inc_bytes": inc_bytes,
                "new_obj": n_new_obj, "shared_obj": n_shared_obj,
            }
        finally:
            shutil.rmtree(client, ignore_errors=True)

    results = []
    with ThreadPoolExecutor(max_workers=8) as ex:
        for res in ex.map(one_pair, enumerate(pairs)):
            if res:
                results.append(res)
                with print_lock:
                    print(f"pair {res['repo']}:{res['from']}->{res['to']} "
                          f"layers={res['layers']} ok={res['all_ok']} "
                          f"inc={res['inc_bytes']/1e6:.1f}MB "
                          f"std={res['std_bytes']/1e6:.1f}MB", flush=True)
    return results


def build_seg_index(rec):
    segs = []
    off = 0
    for seg in rec.segs:
        t, a, b, c, data = seg
        L = len(data) if t == SEG_META else (a if t == SEG_PAD else c)
        segs.append((off, off + L, seg))
        off += L
    return segs, off


def read_span(segs, objraw_by_idx, a, b):
    out = bytearray()
    for (s, e, seg) in segs:
        if e <= a:
            continue
        if s >= b:
            break
        t, x, y, z, data = seg
        if t == SEG_META:
            out += data[max(a, s) - s:min(b, e) - s]
        elif t == SEG_PAD:
            out += b"\x00" * (min(b, e) - max(a, s))
        else:
            raw = objraw_by_idx[x]
            out += raw[y + max(a, s) - s:y + min(b, e) - s]
    return bytes(out)


def inflate_obj(store, sha, raw_len):
    p = os.path.join(store, "objects", sha[:2], sha)
    comp = open(p, "rb").read()
    return zlib.decompressobj(-15).decompress(comp, raw_len + 64)


def load_entry(img_dir):
    for f in os.listdir(img_dir):
        if f.startswith("sha256:") or (f.endswith(".json") and f != "index.json"):
            try:
                cfg = json.load(open(os.path.join(img_dir, f)))
                return cfg.get("config", {}).get("Entrypoint") or \
                    cfg.get("config", {}).get("Cmd") or []
            except Exception:
                return []
    return []


def run_lazy(rec_by_sha, images, limit_ai=4):
    rows = []
    ai_done = 0
    for (repo, tag), img in sorted(images.items()):
        cls = img["class"]
        if cls == "ai":
            if ai_done >= limit_ai:
                continue
            ai_done += 1
        lay_paths = img["lay_paths"]
        if not lay_paths:
            continue
        recs = []
        ok = True
        for lay in img["info"]["layers"]:
            if not lay.get("gz"):
                continue
            dg = lay["digest"].split(":")[-1].split(".tar.gz")[0].split(".")[0]
            r = rec_by_sha.get(dg)
            if not r:
                ok = False
                break
            recs.append(load_recipe(STORE_HY, r["recipe"]))
        if not ok:
            continue

        cache = os.path.join(ROOT, "data", "rootfs_cache",
                             f"{cls}__{repo.replace('/', '_')}__{tag}")
        if not os.path.exists(os.path.join(cache, ".done")):
            extract_rootfs([lp if os.path.isabs(lp) else os.path.join(ROOT, lp)
                            for lp in lay_paths], cache)
            open(os.path.join(cache, ".done"), "w").write("1")

        entry = load_entry(img["dir"])
        boot = q4mod.boot_file_set(cache, entry)
        if not boot:
            continue

        n_files = n_bytes = n_ok = 0
        bad = []
        boot_objs = set()
        all_objs = set()
        tfs = {}
        member_maps = {}
        segs_by_layer = {}
        raws = {}
        try:
            for li, lp in enumerate(lay_paths):
                ap = lp if os.path.isabs(lp) else os.path.join(ROOT, lp)
                tf = tarfile.open(ap, "r:gz")
                tfs[li] = tf
                mm = {}
                for m in tf.getmembers():
                    mm[norm_name(m.name)] = m
                member_maps[li] = mm
            for rec in recs:
                for (sha, raw_len, crc) in rec.objs:
                    all_objs.add(sha)

            for path in sorted(boot):
                src_li = None
                for lj in range(len(lay_paths) - 1, -1, -1):
                    if path in member_maps[lj]:
                        src_li = lj
                        break
                if src_li is None:
                    continue
                m = member_maps[src_li][path]
                rec = recs[src_li]
                if src_li not in segs_by_layer:
                    segs_by_layer[src_li] = build_seg_index(rec)
                segs, _ = segs_by_layer[src_li]
                a0 = m.offset
                b0 = m.offset_data + (m.size if m.isreg() else 0)
                need = {seg[1] for (s, e, seg) in segs
                        if not (e <= a0 or s >= b0) and seg[0] == SEG_REF}
                objraw = {}
                for idx in need:
                    sha = rec.objs[idx][0]
                    objraw[idx] = inflate_obj(STORE_HY, sha, rec.objs[idx][1])
                    boot_objs.add(sha)
                if m.isreg():
                    want = tfs[src_li].extractfile(m).read()
                    got = read_span(segs, objraw, m.offset_data,
                                    m.offset_data + m.size)
                    n_files += 1
                    n_bytes += m.size
                    if got == want:
                        n_ok += 1
                    else:
                        bad.append((path, "data"))
                else:
                    lp_abs = lay_paths[src_li] if os.path.isabs(
                        lay_paths[src_li]) else os.path.join(ROOT,
                                                             lay_paths[src_li])
                    if src_li not in raws:
                        raws[src_li] = gzip.open(lp_abs, "rb")
                    f = raws[src_li]
                    f.seek(m.offset)
                    want = f.read(m.offset_data - m.offset)
                    got = read_span(segs, objraw, m.offset, m.offset_data)
                    n_files += 1
                    if got == want:
                        n_ok += 1
                    else:
                        bad.append((path, "symlink" if m.issym() else "hdr"))
            row = {
                "class": cls, "repo": repo, "tag": tag,
                "boot_files": n_files, "boot_ok": n_ok,
                "boot_bytes": n_bytes,
                "all_ok": n_files > 0 and n_ok == n_files,
                "boot_objs": len(boot_objs), "all_objs": len(all_objs),
                "boot_obj_frac": len(boot_objs) / max(1, len(all_objs)),
                "bad": bad[:5],
            }
            rows.append(row)
            with print_lock:
                print(f"lazy {repo}:{tag} files={n_files} ok={n_ok} "
                      f"bootobj={row['boot_objs']}/{row['all_objs']}"
                      f"{' BAD:' + str(bad[:3]) if bad else ''}", flush=True)
        except Exception as e:
            import traceback
            with print_lock:
                print(f"lazy ERR {repo}:{tag}: {e}\n"
                      f"{traceback.format_exc()[-500:]}", flush=True)
        finally:
            for tf_ in tfs.values():
                tf_.close()
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lazy-ai", type=int, default=4)
    ap.add_argument("--skip-incremental", action="store_true")
    ap.add_argument("--skip-lazy", action="store_true")
    args = ap.parse_args()

    m1 = json.load(open(M1))
    rec_by_sha = {}
    for r in m1["policies"]["hybrid"]:
        if "recipe" in r and r.get("blob_sha"):
            rec_by_sha[r["blob_sha"]] = r

    images = {}
    images_by_repo = {}
    for cls in os.listdir(IMAGES):
        clsdir = os.path.join(IMAGES, cls)
        if not os.path.isdir(clsdir):
            continue
        for name in sorted(os.listdir(clsdir)):
            ip = os.path.join(clsdir, name, "index.json")
            if not os.path.exists(ip):
                continue
            info = json.load(open(ip))
            lay_paths = []
            for lay in info["layers"]:
                if not lay.get("gz"):
                    continue
                rel = os.path.relpath(
                    os.path.join(clsdir, name, lay["file"]), ROOT)
                lay_paths.append(rel)
            img = {"class": cls, "info": info, "lay_paths": lay_paths,
                   "dir": os.path.join(clsdir, name)}
            images[(info["repo"], info["tag"])] = img
            images_by_repo.setdefault(
                (cls, info["repo"]), []).append((info["tag"], img))

    results = {}
    if not args.skip_incremental:
        print("=== A) incrementalcorrectness ===", flush=True)
        t0 = time.time()
        inc = run_incremental(rec_by_sha, images_by_repo)
        results["incremental"] = {
            "pairs": inc,
            "n_pairs": len(inc),
            "n_pairs_ok": sum(1 for p in inc if p["all_ok"]),
            "n_layers": sum(p["layers"] for p in inc),
            "n_layers_ok": sum(p["layers"] for p in inc if p["all_ok"]),
            "inc_bytes": sum(p["inc_bytes"] for p in inc),
            "std_bytes": sum(p["std_bytes"] for p in inc),
        }
        print(f"pairs={results['incremental']['n_pairs']} "
              f"ok={results['incremental']['n_pairs_ok']} "
              f"layers={results['incremental']['n_layers']} "
              f"({time.time()-t0:.0f}s)", flush=True)

    if not args.skip_lazy:
        print("=== B) verification ===", flush=True)
        t0 = time.time()
        lazy = run_lazy(rec_by_sha, images, limit_ai=args.lazy_ai)
        results["lazy"] = {
            "images": lazy,
            "n_images": len(lazy),
            "n_images_ok": sum(1 for r in lazy if r["all_ok"]),
            "total_files": sum(r["boot_files"] for r in lazy),
            "total_ok": sum(r["boot_ok"] for r in lazy),
            "total_bytes": sum(r["boot_bytes"] for r in lazy),
        }
        print(f"images={len(lazy)} "
              f"all_ok={results['lazy']['n_images_ok']} "
              f"files={results['lazy']['total_files']} "
              f"ok={results['lazy']['total_ok']} ({time.time()-t0:.0f}s)",
              flush=True)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(results, open(OUT, "w"), indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
