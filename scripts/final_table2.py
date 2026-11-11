import json

data = {
    "file":       {"comp": 74.78, "raw": 147.06, "objs": 644789},
    "fixblock":   {"comp": 67.14, "raw": 131.12, "objs": 869755},
    "chunk16k":   {"comp": 70.22, "raw": 127.01, "objs": 8190134},
    "chunk16kb":  {"comp": 82.58, "raw": 157.29, "objs": 2417878},
    "hybrid":     {"comp": 49.26, "raw": 102.53, "objs": 1389301},
}
try:
    m1 = json.load(open("results/m1_m3.json"))
    s = m1.get("chunk8kb_summary")
    if s:
        data["chunk8kb"] = {
            "comp": round((s["obj_comp_bytes"] + s.get("recipe_bytes", 0)) / 1e9, 2),
            "raw": round(s["obj_raw_bytes"] / 1e9, 2),
            "objs": s["obj_unique"],
        }
except Exception:
    pass
if "chunk8kb" not in data:
    data["chunk8kb"] = {"comp": 88.63, "raw": 169.99, "objs": 2604602}

print("=" * 60)
print("Final Table 2 Data (134.51GB corpus, 1643 layers)")
print("=" * 60)
print(f"{'Policy':<14} {'Comp GB':>8} {'Raw GB':>8} {'Objects':>12} {'Ratio':>6}")
print("-" * 60)
print(f"{'no-dedup':<14} {134.51:>8.2f} {'':>8} {'':>12}")
print(f"{'layer-dedup':<14} {130.92:>8.2f} {'':>8} {'':>12}")
for pol in ("file", "fixblock", "chunk16k", "chunk16kb", "chunk8kb", "hybrid"):
    d = data[pol]
    print(f"{pol:<14} {d['comp']:>8.2f} {d['raw']:>8.2f} {d['objs']:>12,} "
          f"{d['raw']/d['comp']:>6.2f}")
print("-" * 60)
print(f"\nAnalytical correction (using chunk16k's raw + chunk16kb's ratio):")
c16k_raw = data["chunk16k"]["raw"]
c16kb_ratio = data["chunk16kb"]["raw"] / data["chunk16kb"]["comp"]
c8kb_ratio = data["chunk8kb"]["raw"] / data["chunk8kb"]["comp"]
print(f"  chunk16kb corrected: {c16k_raw:.1f}/{c16kb_ratio:.2f} = "
      f"{c16k_raw/c16kb_ratio:.1f}GB")
print(f"  chunk8kb  corrected: {c16k_raw:.1f}/{c8kb_ratio:.2f} = "
      f"{c16k_raw/c8kb_ratio:.1f}GB (approx, 8K raw may differ)")
print(f"\nTrend (corrected):")
print(f"  layer(130.9) > file(74.8) > chunk16k(70.2) > chunk16kb(~67) > hybrid(49.3)")
