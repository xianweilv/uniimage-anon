#!/usr/bin/env python3

image）:  =
file F =  ELF  + configuration(/etc) + file（directory
input）， rootfs

200Mb/s, 5ms RTT, objecton-demand miss）:
  T_full = image/BW                       （
  T_task = object/BW + |miss|×RTT    （on-demand
incremental（tag i→i+1 :
  T_std  = layer/BW +  miss
  T_inc  = object/BW +  miss
"""
import json
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recipe_reader import load_recipe, extract_rootfs, SEG_META, SEG_PAD, SEG_REF
import run_q4_modes as q4mod

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "data", "images")
STORE = os.path.join(ROOT, "data", "stores", "store_hybrid")
M1 = os.path.join(ROOT, "results", "m1_m3.json")
Q4 = os.path.join(ROOT, "results", "q4_modes.json")
OUT = os.path.join(ROOT, "results", "r2_task.json")

BW = 200e6 / 8
RTT = 0.005

TASKS = {
    "app": {
        "nginx": ("nginx", ["etc/nginx/nginx.conf", "etc/nginx/mime.types",
                            "usr/share/nginx/html/index.html"]),
        "redis": ("redis-server", ["etc/redis/redis.conf"]),
        "postgres": ("postgres", ["var/lib/postgresql/postgresql.conf",
                                  "etc/postgresql/pg_hba.conf"]),
        "mysql": ("mysqld", ["etc/mysql/my.cnf"]),
        "memcached": ("memcached", []),
        "traefik": ("traefik", ["etc/traefik/traefik.yml"]),
        "gitlab-ce": ("gitlab-ctl", ["etc/gitlab/gitlab.rb"]),
        "jenkins": ("java", ["var/jenkins_home/config.xml"]),
        "wordpress": ("apache2", ["var/www/html/wp-config.php",
                                  "var/www/html/index.php"]),
    },
    "base": {
        "python": ("python3", ["usr/local/lib/python*/site-packages/README.txt"]),
        "node": ("node", ["usr/local/lib/node_modules/npm/README.md"]),
        "golang": ("go", ["usr/local/go/VERSION"]),
        "gcc": ("gcc", []),
        "openjdk": ("java", []),
        "eclipse-temurin": ("java", []),
        "php": ("php", ["usr/local/etc/php/php.ini-*"]),
        "ruby": ("ruby", []),
        "perl": ("perl", []),
        "rust": ("cargo", ["usr/local/cargo/config.toml"]),
        "llvm": ("clang", []),
        "gradle": ("java", []),
        "maven": ("java", []),
        "dotnet": ("dotnet", []),
        "haskell": ("ghc", []),
        "erlang": ("erl", []),
    },
    "ai": {
        "pytorch": ("python3", ["usr/local/lib/python*/site-packages/torch/version.py"]),
        "cuda": ("nvidia-smi", []),
        "tensorflow": ("python3", ["usr/local/lib/python*/site-packages/tensorflow/version_info.py"]),
    },
}

import fnmatch


def norm(nm):
    nm = nm.lstrip("/")
    while nm.startswith("./"):
        nm = nm[2:]
    return nm


def build_segs(rec):
    segs = []
    off = 0
    for seg in rec.segs:
        t, a, b, c, data = seg
        L = len(data) if t == SEG_META else (a if t == SEG_PAD else c)
        segs.append((off, off + L, seg))
        off += L
    return segs


def main():
    m1 = json.load(open(M1))
    rec_by_sha = {r["blob_sha"]: r for r in m1["policies"]["hybrid"]
                  if r.get("blob_sha") and "recipe" in r}
    q4 = json.load(open(Q4))
    lazy_by_key = {(r["repo"], r["tag"]): r for r in q4["lazy"]}

    images = []
    for cls in ("base", "ai", "app"):
        clsdir = os.path.join(IMAGES, cls)
        for name in sorted(os.listdir(clsdir)):
            ip = os.path.join(clsdir, name, "index.json")
            if os.path.exists(ip):
                info = json.load(open(ip))
                images.append((cls, info, os.path.join(clsdir, name)))

    import tarfile
    rows = []
    for cls, info, imgdir in images:
        repo_short = info["repo"].split("/")[-1]
        task = TASKS.get(cls, {}).get(repo_short)
        if not task:
            continue
        exe, data_globs = task
        lay_paths = [os.path.join(imgdir, l["file"])
                     for l in info["layers"] if l.get("gz")]
        recs = []
        ok = True
        for lay in info["layers"]:
            if not lay.get("gz"):
                continue
            dg = lay["digest"].split(":")[-1].split(".")[0]
            r = rec_by_sha.get(dg)
            if not r:
                ok = False
                break
            recs.append(load_recipe(STORE, r["recipe"]))
        if not ok:
            continue

        cache = os.path.join(ROOT, "data", "rootfs_cache",
                             f"{cls}__{info['repo'].replace('/', '_')}__{info['tag']}")
        if not os.path.exists(os.path.join(cache, ".done")):
            extract_rootfs(lay_paths, cache)
            open(os.path.join(cache, ".done"), "w").write("1")
        entry = []
        for f in os.listdir(imgdir):
            if f.startswith("sha256:") or (f.endswith(".json")
                                           and f != "index.json"):
                try:
                    cfg = json.load(open(os.path.join(imgdir, f)))
                    entry = cfg.get("config", {}).get("Entrypoint") or \
                        cfg.get("config", {}).get("Cmd") or []
                except Exception:
                    pass
        boot = q4mod.boot_file_set(cache, entry or [exe])
        all_files = {}
        for dirpath, _, names in os.walk(cache):
            for nm in names:
                if nm == ".done":
                    continue
                full = os.path.join(dirpath, nm)
                rel = os.path.relpath(full, cache).replace("\\", "/")
                try:
                    all_files[rel] = os.path.getsize(full)
                except OSError:
                    all_files[rel] = 0
        data_files = set()
        for g in data_globs:
            data_files.update(fnmatch.filter(all_files, g))
        task_files = {f for f in boot if f in all_files}
        task_files |= data_files
        if not task_files:
            continue

        maps = {}
        tfs = {}
        segs_by_layer = {}
        try:
            for li, lp in enumerate(lay_paths):
                tf = tarfile.open(lp, "r:gz")
                tfs[li] = tf
                mm = {}
                for m in tf.getmembers():
                    mm[norm(m.name)] = m
                maps[li] = mm
            for li, rec in enumerate(recs):
                segs_by_layer[li] = build_segs(rec)

            task_obj_bytes = 0
            task_miss = 0
            img_obj_bytes = 0
            for path in sorted(task_files):
                src_li = None
                for lj in range(len(lay_paths) - 1, -1, -1):
                    if norm(path) in maps[lj]:
                        src_li = lj
                        break
                if src_li is None:
                    continue
                m = maps[src_li][norm(path)]
                a0 = m.offset
                b0 = m.offset_data + (m.size if m.isreg() else 0)
                need = {seg[1] for (s, e, seg) in segs_by_layer[src_li]
                        if not (e <= a0 or s >= b0) and seg[0] == SEG_REF}
                for idx in need:
                    sha = recs[src_li].objs[idx][0]
                    p = os.path.join(STORE, "objects", sha[:2], sha)
                    task_obj_bytes += os.path.getsize(p)
                    task_miss += 1
            lz = lazy_by_key.get((info["repo"], info["tag"]))
            full_bytes = sum(l["size"] for l in info["layers"]
                             if l.get("gz"))
            t_full = full_bytes / BW
            t_task = task_obj_bytes / BW + task_miss * RTT
            rows.append({
                "class": cls, "repo": info["repo"], "tag": info["tag"],
                "task_files": len(task_files),
                "task_obj_mb": task_obj_bytes / 1e6, "misses": task_miss,
                "t_full_s": t_full, "t_task_s": t_task,
                "speedup": t_full / t_task if t_task > 0 else 0,
            })
            print(f"{info['repo']}:{info['tag']} files={len(task_files)} "
                  f"obj={task_obj_bytes/1e6:.2f}MB miss={task_miss} "
                  f"T_full={t_full:.1f}s T_task={t_task:.3f}s "
                  f"({t_full/t_task:.0f}x)", flush=True)
        except Exception as e:
            print("ERR", info["repo"], info["tag"], e, flush=True)
        finally:
            for tf in tfs.values():
                tf.close()

    import statistics as st
    summary = {}
    for cls in ("base", "ai", "app"):
        sub = [r for r in rows if r["class"] == cls]
        if sub:
            summary[cls] = {
                "n": len(sub),
                "t_full_med": st.median([r["t_full_s"] for r in sub]),
                "t_task_med": st.median([r["t_task_s"] for r in sub]),
                "speedup_med": st.median([r["speedup"] for r in sub]),
            }
    print("SUMMARY:", json.dumps(summary, indent=1), flush=True)
    json.dump({"images": rows, "summary": summary}, open(OUT, "w"), indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
