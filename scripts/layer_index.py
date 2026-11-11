#!/usr/bin/env python3
output data/layer_index.json（experiment

layer{class, repo, tag, path, comp_size, blob_sha(filesha256)}
"""
import hashlib
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "data", "images")
OUT = os.path.join(ROOT, "data", "layer_index.json")


def main():
    layers = []
    for cls in sorted(os.listdir(IMAGES)):
        clsdir = os.path.join(IMAGES, cls)
        if not os.path.isdir(clsdir):
            continue
        for name in sorted(os.listdir(clsdir)):
            idx_path = os.path.join(clsdir, name, "index.json")
            if not os.path.exists(idx_path):
                continue
            info = json.load(open(idx_path))
            for lay in info["layers"]:
                if not lay.get("gz"):
                    continue
                p = os.path.join(clsdir, name, lay["file"])
                if not os.path.exists(p):
                    continue
                dg = lay["digest"].split(":")[-1].split(".tar.gz")[0].split(".")[0]
                layers.append({
                    "class": cls,
                    "repo": info["repo"],
                    "tag": info["tag"],
                    "image": name,
                    "path": os.path.relpath(p, ROOT),
                    "size": os.path.getsize(p),
                    "blob_sha": "sha256:" + dg,
                })
    layers.sort(key=lambda x: (x["class"], x["repo"], x["tag"], x["path"]))
    with open(OUT, "w") as f:
        json.dump({"layers": layers}, f, indent=0)
    total = sum(l["size"] for l in layers)
    print(f"{len(layers)} gzip layers, {total/1e9:.2f} GB compressed")
    for cls in sorted(set(l["class"] for l in layers)):
        sub = [l for l in layers if l["class"] == cls]
        print(f"  {cls}: {len(sub)} layers, {sum(l['size'] for l in sub)/1e9:.2f} GB")


if __name__ == "__main__":
    main()
