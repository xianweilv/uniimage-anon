#!/usr/bin/env python3

A) incremental tag :
    repo  tag  (i, i+1):
     std_pull_mb    = tag i+1  blob_sha  tag i layerlayersize
     incr_mb        = Σ (object comp size) +  recipe size
     shared_pct     = tag i+1 object tag i object
   :  B  std = std_pull/B, incr = incr/B + N_req × RTT

B) on-demandcold start（:
    rootfs（whiteout  ENTRYPOINT  ELF
   + file boot file F：
     B1  = imagelayer）
     B5  = eStargz : boot file gzip  +
     U   = boot fileobject comp  + recipe
   cold start B, latency R）: bytes/B + |requests|×R

: python3 scripts/run_q4_modes.py [--bw-mbps 200] [--rtt-ms 5]
"""
import argparse
import gzip
import json
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recipe_reader import load_recipe, extract_rootfs

SEG_META, SEG_PAD, SEG_REF = 0, 1, 2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "data", "images")
STORE = os.path.join(ROOT, "data", "stores", "store_hybrid")
OUT = os.path.join(ROOT, "results", "q4_modes.json")

BASE_FILES = [
    "etc/ld.so.cache", "etc/passwd", "etc/group", "etc/nsswitch.conf",
    "etc/resolv.conf", "etc/hosts", "etc/hostname",
    "etc/localtime", "etc/timezone",
    "dev/null", "dev/zero", "dev/random", "dev/urandom", "dev/tty",
    "proc/self", "sys", "tmp", "run", "var/run",
]
LIB_SEARCH = ["lib", "lib64", "usr/lib", "usr/lib64", "usr/local/lib",
              "usr/lib/x86_64-linux-gnu", "lib/x86_64-linux-gnu"]


def parse_elf_needed(path):
    try:
        with open(path, "rb") as f:
            data = f.read(64)
            if data[:4] != b"\x7fELF" or data[4] != 2:
                return None
            f.seek(0)
            hdr = f.read(64)
            e_shoff, = struct.unpack_from("<Q", hdr, 40)
            e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", hdr, 58)
            f.seek(e_shoff)
            shdrs = f.read(e_shentsize * e_shnum)
            secs = []
            for i in range(e_shnum):
                off = i * e_shentsize
                sh_type, = struct.unpack_from("<I", shdrs, off + 4)
                sh_offset, = struct.unpack_from("<Q", shdrs, off + 24)
                sh_size, = struct.unpack_from("<Q", shdrs, off + 32)
                sh_link, = struct.unpack_from("<I", shdrs, off + 40)
                sh_entsize, = struct.unpack_from("<Q", shdrs, off + 56)
                secs.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))
            needed = []
            for sh_type, sh_offset, sh_size, sh_link, sh_entsize in secs:
                if sh_type != 6 or sh_entsize == 0:   # SHT_DYNAMIC
                    continue
                f.seek(sh_offset)
                dyn = f.read(sh_size)
                str_off, str_size, _, _, _ = secs[sh_link]
                f.seek(str_off)
                strtab = f.read(str_size)
                for i in range(0, len(dyn), 16):
                    d_tag, d_val = struct.unpack_from("<QQ", dyn, i)
                    if d_tag == 0:
                        break
                    if d_tag == 1:   # DT_NEEDED
                        end = strtab.find(b"\0", d_val)
                        needed.append(strtab[d_val:end].decode("utf-8", "replace"))
            return needed
    except Exception:
        return None


def boot_file_set(rootfs, entry_cmd):
    boot = set()
    for bf in BASE_FILES:
        p = os.path.join(rootfs, bf)
        if os.path.exists(p) or os.path.islink(p):
            boot.add(bf)
    to_resolve = []
    if entry_cmd:
        exe = entry_cmd[0]
        if not os.path.isabs(exe):
            for d in ("bin", "sbin", "usr/bin", "usr/sbin", "usr/local/bin",
                      "usr/local/sbin"):
                p = os.path.join(rootfs, d, exe)
                if os.path.exists(p):
                    exe = p
                    break
        to_resolve.append(os.path.join(rootfs, exe) if not os.path.isabs(exe) else exe)
        if len(entry_cmd) > 1 and entry_cmd[0] in ("sh", "/bin/sh", "bash", "/bin/bash"):
            to_resolve.append(os.path.join(rootfs, "bin/sh"))

    seen_libs = set()
    queue = list(to_resolve)
    while queue:
        exe = queue.pop()
        exe = os.path.realpath(exe)
        if exe in seen_libs or not os.path.exists(exe):
            continue
        seen_libs.add(exe)
        rel = os.path.relpath(exe, rootfs).replace("\\", "/")
        boot.add(rel)
        needed = parse_elf_needed(exe)
        if needed:
            for lib in needed:
                if lib in seen_libs:
                    continue
                found = None
                for d in LIB_SEARCH:
                    p = os.path.join(rootfs, d, lib)
                    if os.path.exists(p):
                        found = p
                        break
                if found:
                    queue.append(found)
                else:
                    seen_libs.add(lib)
    return boot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bw-mbps", type=float, default=200.0)
    ap.add_argument("--rtt-ms", type=float, default=5.0)
    args = ap.parse_args()

    m1_path = os.path.join(ROOT, "results", "m1_m3.json")
    m1 = json.load(open(m1_path))
    recs = m1["policies"]["hybrid"]

    def norm_key(f):
        f = f.replace("\\", "/")
        if os.path.isabs(f):
            return os.path.normpath(f)
        return os.path.normpath(os.path.join(ROOT, f))

    by_file = {norm_key(r["file"]): r for r in recs}

    def get_rec(lp):
        return by_file.get(norm_key(lp))

    images = {}
    for cls in os.listdir(IMAGES):
        clsdir = os.path.join(IMAGES, cls)
        if not os.path.isdir(clsdir):
            continue
        for name in sorted(os.listdir(clsdir)):
            idx = os.path.join(clsdir, name, "index.json")
            if not os.path.exists(idx):
                continue
            info = json.load(open(idx))
            lay_paths = []
            for lay in info["layers"]:
                if not lay.get("gz"):
                    continue
                rel = os.path.relpath(os.path.join(clsdir, name, lay["file"]), ROOT)
                lay_paths.append(rel)
            images[(info["repo"], info["tag"])] = {
                "class": cls, "info": info, "layers": lay_paths,
                "dir": os.path.join(clsdir, name),
            }

    print("=== A) incremental tag）===")
    incr_results = []
    repos = {}
    for (repo, tag), img in images.items():
        repos.setdefault((img["class"], repo), []).append((tag, img))
    for (cls, repo), tags in sorted(repos.items()):
        def tag_key(t):
            t = t[0]
            nums = [int(x) if x.isdigit() else x for x in
                    __import__("re").findall(r"\d+|\D+", t)]
            return ([(0, x) if isinstance(x, int) else (1, x) for x in nums], t)
        tags.sort(key=tag_key)
        for (t1, img1), (t2, img2) in zip(tags, tags[1:]):
            shas1 = set()
            for lay in img1["info"]["layers"]:
                shas1.add(lay["digest"])
            std_bytes = 0
            total_bytes = 0
            for lay in img2["info"]["layers"]:
                total_bytes += lay["size"]
                if lay["digest"] not in shas1:
                    std_bytes += lay["size"]
            objs1 = set()
            ok = True
            for lp in img1["layers"]:
                r = get_rec(lp)
                if not r or "recipe" not in r:
                    ok = False
                    break
                try:
                    rec = load_recipe(STORE, r["recipe"])
                    objs1 |= rec.object_set()
                except Exception:
                    ok = False
                    break
            if not ok:
                continue
            new_obj_bytes = 0
            obj_total = 0
            hit = 0
            n_req = 0
            recipe_bytes = 0
            for lp in img2["layers"]:
                r = get_rec(lp)
                if not r or "recipe" not in r:
                    ok = False
                    break
                try:
                    rec = load_recipe(STORE, r["recipe"])
                except Exception:
                    ok = False
                    break
                recipe_bytes += r.get("recipe_gz", 0)
                objs = rec.object_set()
                obj_total += len(objs)
                for (sha, raw_len, crc) in rec.objs:
                    if sha in objs1:
                        hit += 1
                    else:
                        p = os.path.join(STORE, "objects", sha[:2], sha)
                        new_obj_bytes += os.path.getsize(p) if os.path.exists(p) else 0
                        n_req += 1
            if not ok:
                continue
            shared_pct = hit / obj_total * 100 if obj_total else 0
            incr_bytes = new_obj_bytes + recipe_bytes
            bw = args.bw_mbps * 1e6 / 8
            std_time = std_bytes / bw
            incr_time = incr_bytes / bw + n_req * args.rtt_ms / 1000
            row = {
                "class": cls, "repo": repo, "from": t1, "to": t2,
                "std_mb": std_bytes / 1e6, "incr_mb": incr_bytes / 1e6,
                "shared_obj_pct": shared_pct,
                "std_time_s": std_time, "incr_time_s": incr_time,
                "speedup": std_time / incr_time if incr_time else 0,
                "n_new_obj": n_req,
            }
            incr_results.append(row)
            print(f"  {cls}/{repo} {t1}->{t2}: std={row['std_mb']:.1f}MB "
                  f"incr={row['incr_mb']:.1f}MB shared={shared_pct:.1f}% "
                  f"speedup={row['speedup']:.1f}x")

    print("=== B) on-demandcold start（===")
    lazy_results = []
    for (repo, tag), img in sorted(images.items()):
        try:
            rootfs = os.path.join(ROOT, "data", "rootfs_tmp")
            files = extract_rootfs(
                [os.path.join(ROOT, lp) for lp in img["layers"]], rootfs)
            entry = []
            cfg_path = None
            for f in os.listdir(img["dir"]):
                if f.startswith("sha256:") or (f.endswith(".json") and f != "index.json"):
                    cfg_path = os.path.join(img["dir"], f)
                    break
            if cfg_path:
                try:
                    cfg = json.load(open(cfg_path))
                    entry = cfg.get("config", {}).get("Entrypoint") or \
                            cfg.get("config", {}).get("Cmd") or []
                except Exception:
                    pass
            boot = boot_file_set(rootfs, entry)
            if not boot:
                continue
            boot_bytes = 0
            for bf in boot:
                fi = files.get(bf)
                if fi and fi.get("size"):
                    boot_bytes += fi["size"]
            b1_bytes = sum(l["size"] for l in img["info"]["layers"])
            import zlib
            b5_bytes = 0
            n_files_b5 = 0
            for bf in boot:
                fi = files.get(bf)
                if not fi or not fi.get("size"):
                    continue
                p = os.path.join(rootfs, bf)
                try:
                    d = open(p, "rb").read()
                except Exception:
                    continue
                co = zlib.compressobj(6, zlib.DEFLATED, 16 + 15)
                b5_bytes += len(co.compress(d) + co.flush())
                n_files_b5 += 1
            b5_bytes += n_files_b5 * 100 + 512
            u_bytes = 0
            u_objs = set()
            u_recipe_bytes = 0
            for lp in img["layers"]:
                r = get_rec(lp)
                if not r or "recipe" not in r:
                    continue
                rec = load_recipe(STORE, r["recipe"])
                u_recipe_bytes += r.get("recipe_gz", 0)
            for lp in img["layers"]:
                r = get_rec(lp)
                if not r or "recipe" not in r:
                    continue
                try:
                    rec = load_recipe(STORE, r["recipe"])
                except Exception:
                    continue
                seg_ranges = []   # (start, end, seg)
                pos = 0
                for seg in rec.segs:
                    t, a, b, c, data = seg
                    ln = len(data) if t == SEG_META else (a if t == SEG_PAD else c)
                    seg_ranges.append((pos, pos + ln, seg))
                    pos += ln
                import tarfile
                tf = tarfile.open(os.path.join(ROOT, lp), "r:gz")
                for m in tf:
                    rel = m.name.lstrip("/")
                    if rel in boot and m.size > 0:
                        s, e = m.offset_data, m.offset_data + m.size
                        for (ss, se, seg) in seg_ranges:
                            if se <= s or ss >= e:
                                continue
                            t, a, b, c, data = seg
                            if t == SEG_REF:
                                sha = rec.objs[a][0]
                                u_objs.add(sha)
                            elif t == SEG_META:
                                u_recipe_bytes += min(e, se) - max(s, ss)
                tf.close()
            for sha in u_objs:
                p = os.path.join(STORE, "objects", sha[:2], sha)
                if os.path.exists(p):
                    u_bytes += os.path.getsize(p)
            u_total = u_bytes + u_recipe_bytes

            bw = args.bw_mbps * 1e6 / 8
            rtt = args.rtt_ms / 1000
            t_b1 = b1_bytes / bw
            t_b5 = b5_bytes / bw + n_files_b5 * rtt
            t_u = u_total / bw + len(u_objs) * rtt

            GZIP_MBPS = 24.0
            t_uncomp_otf = 0.0
            t_uncomp_raw = 0.0
            n_lazy_files = 0
            for bf in boot:
                fi = files.get(bf)
                if not fi or not fi.get("size"):
                    continue
                p = os.path.join(rootfs, bf)
                try:
                    d = open(p, "rb").read()
                except Exception:
                    continue
                n_lazy_files += 1
                co = zlib.compressobj(6, zlib.DEFLATED, 16 + 15)
                clen = len(co.compress(d) + co.flush())
                t_uncomp_otf += max(len(d) / (GZIP_MBPS * 1e6), clen / bw) + rtt
                t_uncomp_raw += len(d) / bw + rtt

            row = {
                "class": img["class"], "repo": repo, "tag": tag,
                "b1_bytes": b1_bytes, "b5_bytes": b5_bytes, "u_bytes": u_total,
                "boot_files": len(boot), "boot_bytes": boot_bytes,
                "u_objs": len(u_objs), "b5_files": n_files_b5,
                "t_b1_s": t_b1, "t_b5_s": t_b5, "t_u_s": t_u,
                "t_ours_s": t_u,
                "t_uncomp_otf_s": t_uncomp_otf,
                "t_uncomp_raw_s": t_uncomp_raw,
                "n_lazy_files": n_lazy_files,
            }
            lazy_results.append(row)
            print(f"  {img['class']}/{repo}:{tag}: b1={b1_bytes/1e6:.0f}MB "
                  f"b5={b5_bytes/1e6:.1f}MB u={u_total/1e6:.1f}MB "
                  f"t: {t_b1:.1f}/{t_b5:.1f}/{t_u:.1f}s "
                  f"3form: ours={t_u:.2f} otf={t_uncomp_otf:.2f} raw={t_uncomp_raw:.2f} "
                  f"boot={len(boot)} files")
        except Exception as e:
            import traceback
            print(f"  skip {repo}:{tag}: {e}")
            traceback.print_exc()

    out = {
        "bw_mbps": args.bw_mbps, "rtt_ms": args.rtt_ms,
        "incremental": incr_results,
        "lazy": lazy_results,
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        json.dump(out, f, indent=0)
    print("saved", OUT)


if __name__ == "__main__":
    main()
