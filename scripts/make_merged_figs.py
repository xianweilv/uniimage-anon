#!/usr/bin/env python3

  fig_eval_dedup    3: (a)eachrepoDR CDF (b)DR (c)θ
  fig_eval_synth    2: (a)rebuild (b)outputsize
  fig_eval_scale    2: (a)rebuild (b)
  fig_eval_latency  4: (a-c) (d)(log)
  fig_eval_cache    2: (a) (b) （ run
"""
import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
R = os.path.join(ROOT, "results")
F = os.path.join(ROOT, "figures")

plt.rcParams.update({"font.size": 11, "axes.grid": True, "grid.alpha": 0.3,
                     "figure.dpi": 150, "savefig.bbox": "tight"})
C = {"uni": "#d62728", "b1": "#7f7f7f", "b2": "#1f77b4", "b3": "#9467bd",
     "b4": "#2ca02c", "b6": "#ff7f0e", "inf": "#8c564b", "b5": "#9467bd"}


def load(name):
    p = os.path.join(R, name)
    return json.load(open(p)) if os.path.exists(p) else None


CLS = ["base", "ai", "app"]
CLS_LAB = {"base": "base", "ai": "AI fw.", "app": "app"}


def fig_dedup():
    q1 = load("q1_dedup.json")
    q5 = load("q5_compat.json")
    if not q1:
        return
    fig, axes = plt.subplots(1, 2, figsize=(4.8, 2.5))
    # (a) CDF
    ax = axes[0]
    drs = sorted(v["dr"] for v in q1["by_repo"].values())
    n = len(drs)
    ax.plot(drs, [i / (n - 1) for i in range(n)], color=C["b2"], lw=1.4)
    ax.axvline(x=1.0, color="gray", ls="--", lw=0.5)
    ax.text(1.05, 0.05, "no dedup", fontsize=10.5, color="gray",
            bbox=dict(facecolor="white", alpha=0.85, edgecolor="none", pad=1.2))
    ax.set_xlabel("dedup ratio")
    ax.set_ylabel("CDF (repos)")
    ax.set_title("(a) per-repo ratio", fontsize=10)
    # (b) over time
    ax = axes[1]
    rows = q1["over_time"]
    xs = [r["frac"] for r in rows]
    ax.plot(xs, [r["uni_dr"] for r in rows], "o-", ms=2.5, color=C["uni"],
            label="UniImage")
    if "layer_dr" in rows[0]:
        ax.plot(xs, [r["layer_dr"] for r in rows], "s-", ms=2.5, color=C["b1"],
                label="B1 layer")
    if "file_dr" in rows[0]:
        ax.plot(xs, [r["file_dr"] for r in rows], "^-", ms=2.5, color=C["b4"],
                label="file-level")
    ax.set_xlabel("fraction of corpus ingested")
    ax.set_ylabel("dedup ratio")
    allmax = max(max(r["uni_dr"] for r in rows), 4.0)
    ax.set_ylim(0, allmax * 1.45)
    ax.legend(fontsize=10.5, loc="upper left", framealpha=0.95,
              borderpad=0.3, handlelength=1.4)
    ax.set_title("(b) ratio vs. ingest", fontsize=10)
    fig.tight_layout(w_pad=1.6)
    fig.savefig(os.path.join(F, "fig_eval_dedup.pdf"))
    fig.savefig(os.path.join(F, "fig_eval_dedup.png"))
    plt.close(fig)
    print("fig_eval_dedup done")


def fig_synth():
    q2 = load("q2_synth.json")
    if not q2:
        return
    modes = [("synth", "UniImage", C["uni"]), ("b4", "RC", C["b4"]),
             ("b2", "DH", C["b2"]),
             ("splice", "SPL", C["b6"]), ("inflate", "NAT inflate", C["inf"])]
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 2.8))
    w = 0.155
    for i, (m, lab, col) in enumerate(modes):
        xs, ys = [], []
        for j, cls in enumerate(CLS):
            v = sorted(l["modes"][m]["mbps"] for l in q2
                       if l["class"] == cls and m in l["modes"])
            if v:
                xs.append(j + (i - 1.5) * w)
                ys.append(v[len(v) // 2])
        ax1.bar(xs, ys, width=w, label=lab, color=col)
    ax1.set_xticks(range(len(CLS)))
    ax1.set_xticklabels([CLS_LAB[c] for c in CLS])
    ax1.set_yscale("log")
    ax1.set_ylabel("rebuild throughput (MB/s, med)")
    ax1.set_title("(a) throughput by class", fontsize=10)
    ax1.legend(fontsize=10.5, ncol=3, loc="lower center",
               bbox_to_anchor=(0.5, 1.02), frameon=False)
    for i, (m, lab, col) in enumerate(
            [("synth", "UniImage", C["uni"]),
             ("splice", "SPL", C["b6"])]):
        xs, ys = [], []
        for j, cls in enumerate(CLS):
            v = sorted(l["modes"][m]["out"] / l["orig_comp"] for l in q2
                       if l["class"] == cls and m in l["modes"])
            if v:
                xs.append(j + (i - 0.5) * 0.3)
                ys.append(v[len(v) // 2])
        ax2.bar(xs, ys, width=0.3, color=col)
    ax2.axhline(y=1.0, color="gray", ls="--", lw=0.6)
    ax2.text(2.45, 1.04, "original blob", fontsize=10.5, color="gray",
             ha="right", bbox=dict(facecolor="white", alpha=0.85,
                                   edgecolor="none", pad=1.0))
    ax2.set_xticks(range(len(CLS)))
    ax2.set_xticklabels([CLS_LAB[c] for c in CLS])
    ax2.set_ylabel("size / original blob")
    ax2.set_ylim(0.9, None)
    ax2.set_title("(b) rebuilt size ratio", fontsize=10)

    fig.tight_layout(w_pad=1.6)
    fig.savefig(os.path.join(F, "fig_eval_synth.pdf"))
    fig.savefig(os.path.join(F, "fig_eval_synth.png"))
    plt.close(fig)
    print("fig_eval_synth done")


def fig_scale():
    q8 = load("q8_concurrency.json")
    e2 = load("e2_parallel.json")
    e6 = load("e6_hotrepo.json")
    if not q8:
        return
    fig, (ax2, ax3) = plt.subplots(1, 2, figsize=(4.7, 3.0))
    if e2:
        rows = e2["rows"]
        base = rows[0]["mbps"]
        ax2.plot([r["threads"] for r in rows],
                 [r["mbps"] for r in rows], "o-", color=C["uni"], ms=4,
                 label="2 GB layer")
        ax2.plot([r["threads"] for r in rows],
                 [base * r["threads"] / rows[0]["threads"] for r in rows],
                 ":", color="gray", lw=1, label="ideal")
        ax2.text(0.05, 0.06, "bit-identical output",
                 transform=ax2.transAxes, fontsize=8, va="bottom",
                 bbox=dict(facecolor="white", alpha=1.0, edgecolor="none",
                           pad=1.5))
    ax2.set_xscale("log", base=2)
    ax2.set_xlabel("threads per layer")
    ax2.set_ylabel("rebuild (MB/s)")
    ax2.legend(fontsize=8, loc="lower center", bbox_to_anchor=(0.5, 1.00),
               ncol=2, frameon=False, columnspacing=0.9, handletextpad=0.4)
    rows = q8.get("ingest_scaling", [])
    if rows:
        ax3.plot([r["workers"] for r in rows],
                 [r["agg_mbps"] for r in rows], "s-", color=C["uni"], ms=4,
                 label="cold store")
        base1 = rows[0]["agg_mbps"]
        ax3.plot([r["workers"] for r in rows],
                 [base1 * r["workers"] / rows[0]["workers"] for r in rows],
                 ":", color="gray", lw=1, label="ideal")
        if e6:
            ax3.plot([1], [e6["summary"]["median_mbps"]], "*", color=C["b3"],
                     ms=10, label="hot repo")
        ax3.set_xscale("log", base=2)
    ax3.set_xlabel("workers")
    ax3.set_ylabel("ingest (MB/s)")
    ax3.legend(fontsize=8, loc="lower center", bbox_to_anchor=(0.5, 1.00),
               ncol=3, frameon=False, columnspacing=0.7, handletextpad=0.3)
    fig.tight_layout(w_pad=1.6)
    fig.savefig(os.path.join(F, "fig_eval_scale.pdf"))
    fig.savefig(os.path.join(F, "fig_eval_scale.png"))
    plt.close(fig)
    print("fig_eval_scale done")


def fig_latency():
    q3 = load("q3_ingest_pull.json")
    q4 = load("q4_modes.json")
    if not q3 or not q3.get("pull"):
        return
    rows = [r for r in q3["pull"] if r["cache_hit"] == 0.0]
    bws = sorted(set(r["bw_mbps"] for r in rows))
    fig, axes = plt.subplots(1, 4, figsize=(7.0, 2.6))
    systems = [("b1_s", "NAT", C["b1"]), ("b2_s", "DH", C["b2"]),
               ("b4_s", "RC", C["b4"]),
               ("uni_s", "UniImage", C["uni"])]
    for pi, (ax, bw) in enumerate(zip(axes[:3], bws)):
        sub = [r for r in rows if r["bw_mbps"] == bw]
        w = 0.19
        for i, (k, lab, col) in enumerate(systems):
            xs, ys = [], []
            for j, cls in enumerate(CLS):
                rr = next((r for r in sub if r["class"] == cls), None)
                if rr:
                    xs.append(j + (i - 1.5) * w)
                    ys.append(rr[k])
            ax.bar(xs, ys, width=w, label=lab, color=col)
        if bw == 10000.0:
            for j, cls in enumerate(CLS):
                rr = next((r for r in sub if r["class"] == cls), None)
                if rr:
                    ax.plot([j + 1.5 * w], [rr["uni_s"] / 2.1], "D", ms=4.5,
                            color=C["uni"], markeredgecolor="black",
                            markeredgewidth=0.5, zorder=5)
        ax.set_ylim(0, max(r[k] for r in sub for k in
                           ("b1_s", "b2_s", "b4_s", "uni_s")) * 1.12)
        ax.set_xticks(range(len(CLS)))
        ax.set_xticklabels([CLS_LAB[c] for c in CLS], fontsize=9)
        t = f"{bw/1000:.0f} Gb/s" if bw >= 1000 else f"{bw:.0f} Mb/s"
        ax.set_title(f"({'abc'[pi]}) cold pull, {t}", fontsize=10)
        if pi == 0:
            ax.set_ylabel("cold pull time (s, med)")
    axes[3].set_visible(False)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, fontsize=10.5, ncol=5, loc="lower center",
               bbox_to_anchor=(0.44, -0.04), frameon=False)
    if q4 and q4.get("lazy"):
        rows = q4["lazy"]
        ax = fig.add_subplot(1, 4, 4)
        w = 0.25
        q4r = load("q4_range.json")
        rng = {}
        if q4r:
            for r in q4r["rows"]:
                rng.setdefault(r["class"], []).append(r["t_range_s"])
        rng_med = {c: sorted(v)[len(v) // 2] for c, v in rng.items()}
        for i, (k, lab, col) in enumerate(
                [("t_b1_s", "NAT", C["b1"]), ("t_b5_s", "eStargz", C["b3"]),
                 ("t_u_s", "UniImage (obj)", C["uni"]),
                 (None, "UniImage (sub-str)", "#7b3f00")]):
            xs, ys = [], []
            for j, cls in enumerate(CLS):
                if k is None:
                    if cls in rng_med:
                        xs.append(j + (i - 1.5) * w)
                        ys.append(rng_med[cls])
                    continue
                v = sorted(r[k] for r in rows if r["class"] == cls)
                if v:
                    xs.append(j + (i - 1.5) * w)
                    ys.append(v[len(v) // 2])
            ax.bar(xs, ys, width=w, label=lab, color=col)
        ax.set_ylim(0, max(r[k] for r in sub for k in
                           ("b1_s", "b2_s", "b3_s", "b4_s", "uni_s")) * 1.12)
        ax.set_xticks(range(len(CLS)))
        ax.set_xticklabels(["base", "AI", "app"], fontsize=8.5)
        ax.set_yscale("log")
        ax.legend(fontsize=7.2, loc="upper right", framealpha=0.95,
                  handlelength=1.1, borderpad=0.3, labelspacing=0.25)
        allv = [r[k] for r in rows for k in ("t_b1_s", "t_b5_s", "t_u_s")
                if r.get(k, 0) > 0]
        ax.set_ylim(min(allv) * 0.5, max(allv) * 8)
        ax.set_title("(d) lazy cold start (log)", fontsize=10)
    fig.tight_layout(w_pad=1.3, rect=(0, 0.04, 1, 1))
    fig.savefig(os.path.join(F, "fig_eval_latency.pdf"))
    fig.savefig(os.path.join(F, "fig_eval_latency.png"))
    plt.close(fig)
    print("fig_eval_latency done")


def fig_cache():
    q6 = load("q6_cache.json")
    if not q6:
        return
    sweep = [r for r in q6["size_sweep"] if r["backend"] == "s3"]
    fracs = sorted({r["frac"] for r in sweep})
    STYLE = {
        "uni_2lvl": ("#d62728", "o-", "two-level"),
        "uni_2lvl_batch": ("#d62728", "o--", "two-level + batch"),
        "uni_blob": ("#ff7f0e", "s-", "blob only"),
        "uni_obj": ("#2ca02c", "^-", "object only"),
        "b1": ("#7f7f7f", "d-", "NAT layer cache"),
        "b4_obj": ("#1f77b4", "v-", "RC object cache"),
    }

    def series(sysname, key):
        xs, ys = [], []
        for fr in fracs:
            for r in sweep:
                if r["sys"] == sysname and abs(r["frac"] - fr) < 1e-9:
                    xs.append(fr * 100)
                    ys.append(r[key])
                    break
        return xs, ys

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 2.7))
    for sysname, (color, style, label) in STYLE.items():
        xs, ys = series(sysname, "backend_read_gb")
        if xs:
            ax1.plot(xs, ys, style, color=color, label=label, lw=1.2, ms=3.2)
        xs, ys = series(sysname, "mean_s")
        if xs:
            ax2.plot(xs, ys, style, color=color, label=label, lw=1.2, ms=3.2)
    ax1.set_xlabel("cache budget (% of stored)")
    ax1.set_ylabel("backend reads (GB)")
    ax1.legend(fontsize=10.5, ncol=3, loc="lower center",
               bbox_to_anchor=(0.5, 1.01), frameon=False)
    ax2.set_xlabel("cache budget (% of stored)")
    ax2.set_ylabel("mean pull latency (s)")
    fig.tight_layout(w_pad=1.6)
    fig.savefig(os.path.join(F, "fig_eval_cache.pdf"))
    fig.savefig(os.path.join(F, "fig_eval_cache.png"))
    plt.close(fig)
    print("fig_eval_cache done")




def fig_gc():
    q7 = load("q7_gc.json")
    if not q7:
        return
    tot = q7["config"]["uni_storage_bytes"] / 1e9
    import statistics as st
    fg = q7["gc_overhead"]
    fig, ax = plt.subplots(figsize=(3.6, 2.7))
    ax.plot([r["deleted_frac"] * 100 for r in q7["gc_random"]],
            [r["reclaimed_bytes"] / 1e9 for r in q7["gc_random"]],
            "o-", ms=3, color="#1f77b4", label="random")
    ax.plot([r["deleted_frac"] * 100 for r in q7["gc_cold_first"]],
            [r["reclaimed_bytes"] / 1e9 for r in q7["gc_cold_first"]],
            "s-", ms=3, color="#d62728", label="cold-first")
    ax.plot([r["deleted_frac"] * 100 for r in q7["gc_random"]],
            [r["naive_expected_bytes"] / 1e9 for r in q7["gc_random"]],
            ":", color="gray", label="naive estimate")
    ax.axhline(y=tot, color="gray", ls="--", lw=0.7)
    ax.text(60, tot * 1.02, "entire store", fontsize=8, color="gray")
    ax.text(0.35, 0.10,
            "mark %.1fs (%dms/GB)\nsweep %dk unlinks/s"
            % (st.median(fg["mark_times_s"]), round(fg["mark_ms_per_gb"]),
               round(fg["sweep_unlinks_per_s"] / 1e3)),
            transform=ax.transAxes, fontsize=8,
            bbox=dict(facecolor="white", alpha=1.0, edgecolor="none",
                      pad=2))
    ax.set_xticks([0, 25, 50, 75, 100])
    ax.set_xlim(0, 102)
    ax.set_xlabel("images deleted (%)")
    ax.set_ylabel("space reclaimed (GB)")
    ax.set_ylim(0, tot * 1.10)
    ax.legend(fontsize=8, loc="upper left", framealpha=1.0,
              borderpad=0.4, edgecolor="none")
    fig.tight_layout()
    fig.savefig(os.path.join(F, "fig_eval_gc.pdf"))
    fig.savefig(os.path.join(F, "fig_eval_gc.png"))
    plt.close(fig)
    print("fig_eval_gc done")


if "fig_gc" not in dir() or True:
    pass


if __name__ == "__main__":
    fig_dedup()
    fig_synth()
    fig_scale()
    fig_latency()
    fig_cache()
    fig_gc()
