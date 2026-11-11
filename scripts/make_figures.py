#!/usr/bin/env python3

paper）：
  fig_mot_overhead      M2: Deflate
  fig_mot_penalty       M3: compressionfile/8K/16K/hybrid）
  fig_eval_dedup_cdf    Q1: deduplication CDF（
  fig_eval_dedup_time   Q1: deduplicationingest
  fig_eval_synth_class  Q2: imagerebuildthroughput（synth/B3/B4/B2/splice/inflate）
  fig_eval_synth_cost   Q2: compression
  fig_eval_pull         Q3: latency（
  fig_eval_lazy_timeline Q4: cold startcomparison
  fig_eval_sens         Q5: sigma/theta
"""
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")
FIG = os.path.join(ROOT, "figures")

plt.rcParams.update({
    "font.size": 9, "axes.grid": True, "grid.alpha": 0.3,
    "figure.dpi": 150, "savefig.bbox": "tight",
})
C = {
    "uni": "#d62728", "b1": "#7f7f7f", "b2": "#1f77b4", "b3": "#9467bd",
    "b4": "#2ca02c", "b6": "#ff7f0e", "inf": "#8c564b",
}


def load(name):
    p = os.path.join(RES, name)
    return json.load(open(p)) if os.path.exists(p) else None


def fig_mot_overhead():
    d = load("m2_decompose.json")
    if not d:
        return
    cats = sorted(set(r["category"] for r in d["runs"]))
    levels = sorted(set(r["level"] for r in d["runs"]))
    fig, axes = plt.subplots(1, len(levels), figsize=(3.2 * len(levels), 2.6),
                             sharey=True)
    for ax, lv in zip(axes, levels):
        x, lz, he, hd = [], [], [], []
        for c in cats:
            rows = [r for r in d["runs"] if r["category"] == c and r["level"] == lv]
            if not rows:
                continue
            best = min(rows, key=lambda r: r["t_zfull_ms"])
            tot = best["t_zfull_ms"]
            lz.append(best["t_lz77_ms"] / tot * 100)
            he.append(best["t_huffenc_ms"] / tot * 100)
            hd.append(best["t_huffdec_ms"] / tot * 100)
            x.append(c)
        ax.bar(x, lz, label="LZ77 match", color=C["b4"])
        ax.bar(x, he, bottom=lz, label="Huffman enc", color=C["uni"])
        ax.set_title(f"level {lv}")
        ax.set_ylabel("% of full deflate")
        ax.tick_params(axis="x", rotation=20)
    axes[0].legend(fontsize=7)
    fig.suptitle("Deflate time decomposition (LZ77 vs Huffman)", y=1.02)
    fig.savefig(os.path.join(FIG, "fig_mot_overhead.pdf"))
    fig.savefig(os.path.join(FIG, "fig_mot_overhead.png"))
    plt.close(fig)
    print("fig_mot_overhead done")


def fig_mot_penalty():
    d = load("m1_m3.json")
    if not d:
        return
    import collections
    pens = collections.defaultdict(lambda: collections.defaultdict(list))
    for pol in ("file", "chunk8k", "chunk16k", "hybrid"):
        for r in d["layer_records"].get(pol, []):
            cls = next((c for c in ("base", "ai", "app") if f"/{c}/" in r["file"]), "?")
            if r["comp"] and r.get("sum_ref_comp"):
                pens[cls][pol].append(r["sum_ref_comp"] / r["comp"] - 1)
    pols = ["file", "chunk8k", "chunk16k", "hybrid"]
    labels = ["file-level", "chunk 8K", "chunk 16K", "hybrid (UniImage)"]
    cls_list = [c for c in ("base", "ai", "app") if pens[c]]
    if not cls_list:
        return
    fig, ax = plt.subplots(figsize=(4.5, 2.6))
    w = 0.2
    for i, pol in enumerate(pols):
        xs, ys = [], []
        for j, cls in enumerate(cls_list):
            v = pens[cls].get(pol, [])
            if v:
                xs.append(j + (i - 1.5) * w)
                ys.append(sum(v) / len(v) * 100)
        ax.bar(xs, ys, width=w, label=labels[i],
               color=[C["b2"], C["b6"], C["b3"], C["uni"]][i])
    ax.set_xticks(range(len(cls_list)))
    ax.set_xticklabels(cls_list)
    ax.set_ylabel("compression-ratio penalty (%)")
    ax.set_title("Independent-coding penalty vs whole-layer gzip")
    ax.legend(fontsize=7)
    fig.savefig(os.path.join(FIG, "fig_mot_penalty.pdf"))
    fig.savefig(os.path.join(FIG, "fig_mot_penalty.png"))
    plt.close(fig)
    print("fig_mot_penalty done")


def fig_eval_synth_class():
    d = load("q2_synth.json")
    if not d:
        return
    modes = [("synth", "UniImage", C["uni"]), ("b3_simenc", "B3 SimEnc", C["b3"]),
             ("b4", "B4 recomp", C["b4"]), ("b2", "B2 DupHunter", C["b2"]),
             ("splice", "B6 splice", C["b6"]), ("inflate", "inflate (ref)", C["inf"])]
    cls_list = [c for c in ("base", "ai", "app") if any(l["class"] == c for l in d)]
    fig, ax = plt.subplots(figsize=(5.2, 2.8))
    w = 0.13
    for i, (m, lab, col) in enumerate(modes):
        xs, ys = [], []
        for j, cls in enumerate(cls_list):
            v = sorted(l["modes"][m]["mbps"] for l in d
                       if l["class"] == cls and m in l["modes"])
            if v:
                xs.append(j + (i - 2.5) * w)
                ys.append(v[len(v) // 2])
        if xs:
            ax.bar(xs, ys, width=w, label=lab, color=col)
    ax.set_xticks(range(len(cls_list)))
    ax.set_xticklabels(cls_list)
    ax.set_ylabel("layer rebuild throughput (MB/s, med)")
    ax.set_yscale("log")
    ax.legend(fontsize=7, ncol=3, loc="lower center",
              bbox_to_anchor=(0.5, 1.0), frameon=False)
    ax.set_title("")
    fig.savefig(os.path.join(FIG, "fig_eval_synth_class.pdf"))
    fig.savefig(os.path.join(FIG, "fig_eval_synth_class.png"))
    plt.close(fig)
    print("fig_eval_synth_class done")


def fig_eval_pull():
    d = load("q3_ingest_pull.json")
    if not d or not d.get("pull"):
        return
    rows = d["pull"]
    cold = [r for r in rows if r["cache_hit"] == 0.0]
    bws = sorted(set(r["bw_mbps"] for r in cold))
    fig, axes = plt.subplots(1, len(bws), figsize=(3.0 * len(bws), 2.6), sharey=False)
    if len(bws) == 1:
        axes = [axes]
    for ax, bw in zip(axes, bws):
        sub = [r for r in cold if r["bw_mbps"] == bw]
        cls_list = [c for c in ("base", "ai", "app") if any(r["class"] == c for r in sub)]
        systems = [("b1_s", "B1 native", C["b1"]), ("b2_s", "B2 DupHunter", C["b2"]),
                   ("b3_s", "B3 SimEnc", C["b3"]), ("b4_s", "B4 recomp", C["b4"]),
                   ("uni_s", "UniImage", C["uni"])]
        w = 0.15
        for i, (k, lab, col) in enumerate(systems):
            xs, ys = [], []
            for j, cls in enumerate(cls_list):
                rr = next((r for r in sub if r["class"] == cls), None)
                if rr:
                    xs.append(j + (i - 2) * w)
                    ys.append(rr[k])
            ax.bar(xs, ys, width=w, label=lab, color=col)
        ax.set_xticks(range(len(cls_list)))
        ax.set_xticklabels(cls_list)
        ax.set_title(f"{bw/1000:.0f} Gbps" if bw >= 1000 else f"{bw:.0f} Mbps")
        ax.set_ylabel("cold pull time (s, med)")
    handles, labels_ = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels_, fontsize=7, ncol=5, loc="lower center",
               bbox_to_anchor=(0.5, -0.06), frameon=False)
    fig.savefig(os.path.join(FIG, "fig_eval_pull.pdf"))
    fig.savefig(os.path.join(FIG, "fig_eval_pull.png"))
    plt.close(fig)
    print("fig_eval_pull done")


def fig_eval_lazy():
    d = load("q4_modes.json")
    if not d or not d.get("lazy"):
        return
    rows = d["lazy"]
    cls_list = [c for c in ("base", "ai", "app") if any(r["class"] == c for r in rows)]
    fig, ax = plt.subplots(figsize=(4.6, 2.6))
    w = 0.25
    for i, (k, lab, col) in enumerate([("t_b1_s", "B1 full pull", C["b1"]),
                                       ("t_b5_s", "B5 eStargz", C["b3"]),
                                       ("t_u_s", "UniImage lazy", C["uni"])]):
        xs, ys = [], []
        for j, cls in enumerate(cls_list):
            v = sorted(r[k] for r in rows if r["class"] == cls)
            if v:
                xs.append(j + (i - 1) * w)
                ys.append(v[len(v) // 2])
        ax.bar(xs, ys, width=w, label=lab, color=col)
    ax.set_xticks(range(len(cls_list)))
    ax.set_xticklabels(cls_list)
    ax.set_yscale("log")
    ax.set_ylabel("cold-start time (s, med, log)")
    ax.set_title(f"Cold start @ {d['bw_mbps']:.0f} Mbps")
    ax.legend(fontsize=7, loc="upper right")
    fig.savefig(os.path.join(FIG, "fig_eval_lazy_timeline.pdf"))
    fig.savefig(os.path.join(FIG, "fig_eval_lazy_timeline.png"))
    plt.close(fig)
    print("fig_eval_lazy done")


def fig_eval_sens():
    d = load("q5_compat.json")
    if not d or not d.get("sensitivity"):
        return
    sens = d["sensitivity"]
    sig = [(k, v) for k, v in sens.items() if k.startswith("sigma")]
    th = [(k, v) for k, v in sens.items() if k.startswith("theta")]
    if not sig and not th:
        return
    fig, axes = plt.subplots(1, 2, figsize=(6.4, 2.6))
    fig.subplots_adjust(wspace=0.62)
    if sig:
        xs = [k.replace("sigma", "") for k, _ in sig]
        objs = [v["obj_unique"] for _, v in sig]
        ratio = [v["ratio_vs_nodedup"] * 100 for _, v in sig]
        ax2 = axes[0].twinx()
        axes[0].bar(xs, objs, color=C["b2"], alpha=0.7, label="unique objects")
        ax2.plot(xs, ratio, "o-", color=C["uni"], label="stored %")
        axes[0].set_yscale("log")
        axes[0].set_xlabel("inline threshold σ (B)")
        axes[0].set_ylabel("unique objects (log)")
        ax2.set_ylabel("stored bytes / no-dedup (%)", color=C["uni"])
    if th:
        xs = [k.replace("theta", "") for k, _ in th]
        objs = [v["obj_unique"] for _, v in th]
        ratio = [v["ratio_vs_nodedup"] * 100 for _, v in th]
        ax2 = axes[1].twinx()
        axes[1].bar(xs, objs, color=C["b2"], alpha=0.7)
        ax2.plot(xs, ratio, "o-", color=C["uni"])
        axes[1].set_yscale("log")
        axes[1].set_xlabel("batch/CDC threshold θ (B)")
        axes[1].set_ylabel("")
        ax2.set_ylabel("stored bytes / no-dedup (%)", color=C["uni"])
    fig.savefig(os.path.join(FIG, "fig_eval_sens.pdf"))
    fig.savefig(os.path.join(FIG, "fig_eval_sens.png"))
    plt.close(fig)
    print("fig_eval_sens done")


def fig_eval_dedup_time():
    d = load("q1_dedup.json")
    if not d or not d.get("over_time"):
        return
    rows = d["over_time"]
    fig, ax = plt.subplots(figsize=(4.4, 2.6))
    xs = [r["frac"] for r in rows]
    ax.plot(xs, [r["uni_dr"] for r in rows], "o-", color=C["uni"], label="UniImage")
    if "layer_dr" in rows[0]:
        ax.plot(xs, [r["layer_dr"] for r in rows], "s-", color=C["b1"],
                label="layer-level (B1)")
    ax.set_xlabel("fraction of corpus ingested")
    ax.set_ylabel("physical dedup ratio (logical/stored)")
    ax.legend(fontsize=7)
    ax.set_title("Deduplication ratio vs ingest progress")
    fig.savefig(os.path.join(FIG, "fig_eval_dedup_time.pdf"))
    fig.savefig(os.path.join(FIG, "fig_eval_dedup_time.png"))
    plt.close(fig)
    print("fig_eval_dedup_time done")


def main():
    os.makedirs(FIG, exist_ok=True)
    for fn in (fig_mot_overhead, fig_mot_penalty, fig_eval_synth_class,
               fig_eval_pull, fig_eval_lazy, fig_eval_sens, fig_eval_dedup_time):
        try:
            fn()
        except Exception as e:
            print(f"{fn.__name__}: SKIP ({e})")


if __name__ == "__main__":
    main()
