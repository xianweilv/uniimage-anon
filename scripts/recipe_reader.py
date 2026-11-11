#!/usr/bin/env python3

- RecipeReader:  store  recipe（gzip +
- extract_rootfs: layer tar layer（ whiteout directory，
  fileeachfilebelonging toobjectUniImage
"""
import gzip
import io
import zlib
import os
import struct
import tarfile

SEG_META, SEG_PAD, SEG_REF = 0, 1, 2


class Recipe:
    def __init__(self, objs, segs, blob_bytes):
        self.objs = objs       # [(sha_hex, raw_len, crc)]
        self.segs = segs       # [(type, a, b, c, data_or_None)]
        self.blob_bytes = blob_bytes

    def object_set(self):
        return set(o[0] for o in self.objs)

    def walk_files(self, obj_raw_getter):
        off = 0
        for seg in self.segs:
            t, a, b, c, data = seg
            if t == SEG_META:
                yield off, "meta", data
                off += len(data)
            elif t == SEG_PAD:
                yield off, "pad", a
                off += a
            else:
                yield off, "ref", (a, b, c)   # obj_idx, off, len
                off += c


def load_recipe(store, sha):
    p = os.path.join(store, "recipes", sha[:2], sha)
    d = open(p, "rb").read()
    if d[0] == 0x1F and d[1] == 0x8B:
        raw = gzip.decompress(d)   #  recipe  gzip
    else:
        raw = zlib.decompress(d)   #  zlib
    if raw[:4] != b"UIR1":
        raise ValueError("bad recipe magic")
    off = 4
    n_objs, n_segs = struct.unpack_from("<II", raw, off)
    off += 8
    objs = []
    for _ in range(n_objs):
        sha_bin = raw[off:off + 32]
        off += 32
        raw_len, crc = struct.unpack_from("<QI", raw, off)
        off += 12
        objs.append((sha_bin.hex(), raw_len, crc))
    segs = []
    for _ in range(n_segs):
        t = raw[off]; off += 1
        if t == SEG_META:
            (n,) = struct.unpack_from("<I", raw, off)
            off += 4
            data = raw[off:off + n]
            off += n
            segs.append((SEG_META, n, 0, 0, data))
        elif t == SEG_PAD:
            (z,) = struct.unpack_from("<I", raw, off)
            off += 4
            segs.append((SEG_PAD, z, 0, 0, None))
        else:
            idx, o, l = struct.unpack_from("<III", raw, off)
            off += 12
            segs.append((SEG_REF, idx, o, l, None))
    return Recipe(objs, segs, len(raw))


def extract_rootfs(layers, outdir, index_paths=None):
    layers: layer tar.gz layer
    returnfile {path: {"size":n, "layer":i}}"""
    import shutil
    if os.path.exists(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    files = {}

    def safepath(name):
        name = name.lstrip("/")
        parts = []
        for p in name.split("/"):
            if p in ("", ".", ".."):
                continue
            parts.append(p)
        return os.path.join(*parts) if parts else ""

    for i, lp in enumerate(layers):
        tf = tarfile.open(lp, "r:gz")
        whiteouts = []
        for m in tf:
            if not m.name:
                continue
            base = os.path.basename(m.name)
            dirname = os.path.dirname(m.name)
            if base == ".wh..wh..opq":
                d = os.path.join(outdir, safepath(dirname))
                if os.path.isdir(d):
                    for e in os.listdir(d):
                        ep = os.path.join(d, e)
                        if os.path.isdir(ep) and not os.path.islink(ep):
                            shutil.rmtree(ep, ignore_errors=True)
                        else:
                            os.remove(ep)
                        rp = os.path.relpath(ep, outdir).replace("\\", "/")
                        files.pop(rp, None)
                continue
            if base.startswith(".wh."):
                whiteouts.append((dirname, base[4:]))
                continue
            dst = os.path.join(outdir, safepath(m.name))
            if m.isdir():
                os.makedirs(dst, exist_ok=True)
                continue
            if m.issym() or m.islnk():
                os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
                try:
                    if os.path.lexists(dst):
                        os.remove(dst)
                    os.symlink(m.linkname, dst)
                    files[safepath(m.name)] = {"size": 0, "layer": i, "link": m.linkname}
                except OSError:
                    pass
                continue
            os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
            f = tf.extractfile(m)
            if f is None:
                continue
            try:
                if os.path.lexists(dst):
                    os.remove(dst)
                with open(dst, "wb") as w:
                    w.write(f.read())
                files[safepath(m.name)] = {"size": m.size, "layer": i}
            except OSError:
                pass
        for dirname, target in whiteouts:
            tp = os.path.join(outdir, safepath(dirname), target) if safepath(dirname) \
                else os.path.join(outdir, target)
            if os.path.lexists(tp):
                if os.path.isdir(tp) and not os.path.islink(tp):
                    shutil.rmtree(tp, ignore_errors=True)
                else:
                    os.remove(tp)
                rp = os.path.relpath(tp, outdir).replace("\\", "/")
                files.pop(rp, None)
    return files
