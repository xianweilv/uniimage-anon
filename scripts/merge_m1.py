#!/usr/bin/env python3
import json
import sys

base_path, extra_path = sys.argv[1], sys.argv[2]
base = json.load(open(base_path))
extra = json.load(open(extra_path))

for pol, recs in extra["policies"].items():
    if pol not in base["policies"]:
        base["policies"][pol] = recs
        print(f"merged policy {pol}: {len(recs)} records")
for k, v in extra.items():
    if k.endswith("_summary") and k not in base:
        base[k] = v
        print(f"merged {k}")
if extra.get("n_layers") != base.get("n_layers"):
    print(f"WARNING: n_layers differ {base.get('n_layers')} vs {extra.get('n_layers')}")
json.dump(base, open(base_path, "w"), indent=0)
print("saved", base_path)
