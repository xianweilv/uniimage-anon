#!/usr/bin/env python3
import json, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
R = os.path.join(ROOT, "results")
F = os.path.join(ROOT, "figures")

plt.rcParams.update({"font.size": 9, "axes.grid": True, "grid.alpha": 0.3,
                     "figure.dpi": 150, "savefig.bbox": "tight"})

# ============ 1. fig_eval_dedup_cdf ============
q1 = json.load(open(os.path.join(R, "q1_dedup.json")))
drs = sorted(v["dr"] for v in q1["by_repo"].values())
n = len(drs)
fig, ax = plt.subplots(figsize=(3.5, 2.5))
ax.plot([d for d in drs], [i/(n-1) for i in range(n)], 'b-', lw=1.5)
ax.set_xlabel("Physical deduplication ratio")
ax.set_ylabel("CDF (repositories)")
ax.set_title("Per-repository dedup ratio (D2)")
ax.axvline(x=1.0, color='gray', ls='--', lw=0.5)
ax.text(1.05, 0.05, 'no dedup', fontsize=7, color='gray',
        bbox=dict(facecolor='white', alpha=0.85, edgecolor='none', pad=1.2))
fig.savefig(os.path.join(F, "fig_eval_dedup_cdf.pdf"))
fig.savefig(os.path.join(F, "fig_eval_dedup_cdf.png"))
plt.close()
print("fig_eval_dedup_cdf done")

# ============ 2. fig_eval_ingest_time ============
m1 = json.load(open(os.path.join(R, "m1_m3.json")))
recs = m1["policies"]["hybrid"]
times = []
for r in recs:
    times.append(r.get("t_wall_ms", 0) / 1000.0)
fig, ax = plt.subplots(figsize=(3.5, 2.5))
ax.bar(range(len(times)), times, width=1.0, color='steelblue', edgecolor='none')
ax.set_xlabel("Image (push order)")
ax.set_ylabel("Ingest time (s)")
ax.set_title("Per-image ingest time")
n_base = sum(1 for r in recs if '/base/' in r["file"])
n_ai = sum(1 for r in recs if '/ai/' in r["file"])
if n_base:
    ax.axvline(x=n_base-0.5, color='red', ls=':', lw=0.8)
    ax.text(n_base/2, max(times)*0.9, 'base', ha='center', fontsize=7, color='red')
if n_ai:
    ax.text(n_base+n_ai/2, max(times)*0.9, 'AI', ha='center', fontsize=7, color='red')
    ax.axvline(x=n_base+n_ai-0.5, color='red', ls=':', lw=0.8)
    ax.text(n_base+n_ai+(len(times)-n_base-n_ai)/2, max(times)*0.9, 'app',
            ha='center', fontsize=7, color='red')
fig.savefig(os.path.join(F, "fig_eval_ingest_time.pdf"))
fig.savefig(os.path.join(F, "fig_eval_ingest_time.png"))
plt.close()
print("fig_eval_ingest_time done")

# ============ 3. fig_mot_concat_example ============
fig, ax = plt.subplots(figsize=(3.5, 2.2))
ax.set_xlim(0, 10)
ax.set_ylim(0, 5)
ax.axis('off')

# object A
ax.add_patch(mpatches.FancyBboxPatch((0.3, 3.2), 4.0, 1.2,
             boxstyle="round,pad=0.1", fc='lightblue', ec='navy', lw=1.2))
ax.text(2.3, 3.8, "Object A tokens\nLZ77 + Huffman$_A$", ha='center', va='center',
        fontsize=7, style='italic')
ax.text(2.3, 3.05, "self-contained deflate stream", ha='center', fontsize=6, color='gray')

# object B
ax.add_patch(mpatches.FancyBboxPatch((5.7, 3.2), 4.0, 1.2,
             boxstyle="round,pad=0.1", fc='lightgreen', ec='darkgreen', lw=1.2))
ax.text(7.7, 3.8, "Object B tokens\nLZ77 + Huffman$_B$", ha='center', va='center',
        fontsize=7, style='italic')
ax.text(7.7, 3.05, "self-contained deflate stream", ha='center', fontsize=6, color='gray')

ax.annotate("", xy=(2.3, 2.5), xytext=(2.3, 2.95),
            arrowprops=dict(arrowstyle='->', color='navy', lw=1))
ax.annotate("", xy=(7.7, 2.5), xytext=(7.7, 2.95),
            arrowprops=dict(arrowstyle='->', color='darkgreen', lw=1))
ax.text(2.3, 2.7, "Huffman decode", ha='center', fontsize=6, color='navy')
ax.text(7.7, 2.7, "Huffman decode", ha='center', fontsize=6, color='darkgreen')

ax.add_patch(mpatches.FancyBboxPatch((0.3, 1.0), 9.4, 0.8,
             boxstyle="round,pad=0.1", fc='wheat', ec='orange', lw=1.2))
ax.text(2.8, 1.4, "tokens$_A$", ha='center', fontsize=7, color='navy')
ax.text(7.2, 1.4, "tokens$_B$", ha='center', fontsize=7, color='darkgreen')
ax.text(5.0, 0.7, "concatenated token stream (no LZ77 re-run)", ha='center',
        fontsize=6, color='gray')

ax.annotate("", xy=(5.0, 0.15), xytext=(5.0, 0.55),
            arrowprops=dict(arrowstyle='->', color='orange', lw=1.5))
ax.text(5.5, 0.3, "Huffman encode", fontsize=6, color='orange')

# output
ax.text(5.0, 0.02, "standard gzip output (byte-identical uncompressed stream)",
        ha='center', fontsize=6, fontweight='bold')

fig.savefig(os.path.join(F, "fig_mot_concat_example.pdf"))
fig.savefig(os.path.join(F, "fig_mot_concat_example.png"))
plt.close()
print("fig_mot_concat_example done")
