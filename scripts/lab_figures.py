#!/usr/bin/env python3
"""Figures and tables for the cache-lab results (results/lab/*.json).

Writes docs/figures/lab_*.png and docs/LAB_RESULTS.md. Every number comes from the JSON
files produced by lab/experiments/run_heldout.sh.
"""
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results", "lab")
FIG = os.path.join(ROOT, "docs", "figures")
os.makedirs(FIG, exist_ok=True)

TRACES = [
    ("w18test", "Wikipedia 2018 (held-out, 40M req)"),
    ("w19upload", "Wikimedia 2019 upload (40M req)"),
    ("w19text", "Wikimedia 2019 text (10M req)"),
]
DEV = ("w18dev", "Wikipedia 2018 dev slice (10M req, used for design)")

# Validated categorical palette (dataviz reference palette, light mode), fixed order by entity.
SERIES = [
    ("SHADE", "#2a78d6"),
    ("GDSF", "#eb6834"),
    ("LHD", "#1baf7a"),
    ("AdaptSize", "#eda100"),
    ("S3-FIFO", "#e87ba4"),
    ("W-TinyLFU", "#008300"),
    ("LRU", "#6250d6"),
]
BASELINES = ["LRU", "FIFO", "SIEVE", "S3-FIFO", "GDSF", "W-TinyLFU", "AdaptSize", "LHD"]
INK, INK2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"


def load(name):
    path = os.path.join(RES, name + ".json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def table(d, metric="objectMissRatio"):
    t = {}
    for r in d["results"]:
        t.setdefault(r["policy"], {})[r["cacheFrac"]] = r[metric]
    return t


def style(ax):
    ax.set_facecolor(SURFACE)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def miss_curves(metric, fname, ylabel, shade_name="SHADE"):
    traces = [(k, lbl) for k, lbl in TRACES if load(k + "_main")]
    if not traces:
        return
    fig, axes = plt.subplots(1, len(traces), figsize=(5.2 * len(traces), 4.4), facecolor=SURFACE)
    if len(traces) == 1:
        axes = [axes]
    for ax, (key, label) in zip(axes, traces):
        d = load(key + "_main")
        t = table(d, metric)
        sizes = sorted({r["cacheFrac"] for r in d["results"]})
        xs = [100 * s for s in sizes]
        bound = {b["cacheFrac"]: b[metric] for b in d["bounds"]}
        ax.plot(xs, [bound[s] for s in sizes], color=INK2, linestyle="--", linewidth=1.5)
        ax.annotate("PFOO-L bound", (xs[-1], bound[sizes[-1]]), xytext=(4, -7), textcoords="offset points",
                    fontsize=8, color=INK2, va="center")
        if "Belady-Size*" in t:
            ax.plot(xs, [t["Belady-Size*"][s] for s in sizes], color=INK2, linestyle=":", linewidth=1.5)
            ax.annotate("Belady-Size*", (xs[-1], t["Belady-Size*"][sizes[-1]]), xytext=(4, 6),
                        textcoords="offset points", fontsize=8, color=INK2, va="center")
        for name, color in SERIES:
            key_name = shade_name if name == "SHADE" else name
            if key_name not in t:
                continue
            ys = [t[key_name][s] for s in sizes]
            lw = 2.6 if name == "SHADE" else 1.6
            ax.plot(xs, ys, color=color, linewidth=lw, marker="o", markersize=4, label=key_name,
                    zorder=5 if name == "SHADE" else 3)
            if name in ("SHADE", "GDSF", "LRU"):
                ax.annotate(key_name, (xs[0], ys[0]), xytext=(-6, 0), textcoords="offset points",
                            fontsize=8.5, color=INK, ha="right", va="center",
                            fontweight="bold" if name == "SHADE" else "normal")
        ax.set_xscale("log")
        ax.set_xticks(xs)
        ax.set_xticklabels([f"{x:g}%" for x in xs])
        ax.set_xlim(xs[0] / 2.2, xs[-1] * 2.4)
        ax.set_title(label, fontsize=10.5, color=INK, loc="left")
        ax.set_xlabel("cache size (% of trace footprint)", fontsize=9, color=INK2)
        style(ax)
    axes[0].set_ylabel(ylabel, fontsize=9.5, color=INK2)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=len(labels), frameon=False, fontsize=9)
    fig.tight_layout(rect=(0, 0.07, 1, 1))
    fig.savefig(os.path.join(FIG, fname), dpi=160, facecolor=SURFACE)
    plt.close(fig)


def best_baseline(t, size):
    return min(((t[b][size], b) for b in BASELINES if b in t and size in t[b]))


def improvement_rows(metric="objectMissRatio", shade="SHADE"):
    rows = []
    for key, label in TRACES + [DEV]:
        d = load(key + "_main")
        if not d:
            continue
        t = table(d, metric)
        for s in sorted({r["cacheFrac"] for r in d["results"]}):
            bb, bname = best_baseline(t, s)
            sh = t[shade][s]
            rows.append((key, label, s, sh, bb, bname, 100 * (bb - sh) / bb, t["LRU"][s]))
    return rows


def improvement_figure():
    rows = [r for r in improvement_rows() if r[0] != "w18dev"]
    if not rows:
        return
    keys = [k for k, _ in TRACES if any(r[0] == k for r in rows)]
    fig, axes = plt.subplots(1, len(keys), figsize=(4.2 * len(keys), 3.8), facecolor=SURFACE, sharey=True)
    if len(keys) == 1:
        axes = [axes]
    for ax, key in zip(axes, keys):
        sub = [r for r in rows if r[0] == key]
        xs = list(range(len(sub)))
        vals = [r[6] for r in sub]
        ax.bar(xs, vals, color=["#2a78d6" if v >= 0 else "#eb6834" for v in vals], width=0.66,
               edgecolor=SURFACE, linewidth=2)
        for x, v in zip(xs, vals):
            ax.annotate(f"{v:+.1f}%", (x, v), xytext=(0, 3 if v >= 0 else -11), textcoords="offset points",
                        ha="center", fontsize=8, color=INK)
        ax.axhline(0, color=INK2, linewidth=0.8)
        ax.set_xticks(xs)
        ax.set_xticklabels([f"{100 * r[2]:g}%" for r in sub], fontsize=8.5)
        names = sorted({r[5] for r in sub})
        ax.set_title(f"{dict(TRACES)[key].split(' (')[0]}\nbest baseline: {', '.join(names)}", fontsize=9.5,
                     color=INK, loc="left")
        ax.set_xlabel("cache size", fontsize=8.5, color=INK2)
        style(ax)
    axes[0].set_ylabel("fewer misses than best baseline (%)", fontsize=9, color=INK2)
    fig.suptitle("SHADE vs the best of 8 baselines (held-out traces; blue = SHADE better, orange = worse)",
                 fontsize=10.5, color=INK, x=0.01, ha="left")
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "lab_improvement.png"), dpi=160, facecolor=SURFACE)
    plt.close(fig)


ABL_ORDER = [
    ("SHADE", "SHADE (full)"),
    ("SHADE-noAdmit", "no admission control"),
    ("SHADE:exchange=0", "threshold admission (no exchange test)"),
    ("SHADE:slack=1", "exchange margin 1 (untuned)"),
    ("SHADE:index=0:h=4", "fixed horizon instead of index"),
    ("SHADE:km=0", "no censoring correction"),
    ("SHADE:shrink=0", "no shrinkage"),
    ("SHADE:pool=0", "no eviction pool"),
    ("SHADE-noGap", "no gap feature"),
    ("SHADE-noCount", "no count feature"),
    ("SHADE-noFeatures", "no features (one class)"),
]


def ablation_rows():
    out = []
    for key, label in TRACES:
        d = load(key + "_ablation")
        if not d:
            continue
        t = table(d)
        for s in sorted({r["cacheFrac"] for r in d["results"]}):
            base = t["SHADE"][s]
            for pol, desc in ABL_ORDER:
                if pol in t:
                    out.append((key, label, s, pol, desc, t[pol][s], 100 * (t[pol][s] - base) / base))
    return out


def ablation_figure():
    rows = ablation_rows()
    if not rows:
        return
    traces = [k for k, _ in TRACES if any(r[0] == k for r in rows)]
    fig, axes = plt.subplots(1, len(traces), figsize=(5.0 * len(traces), 4.6), facecolor=SURFACE, sharey=True)
    if len(traces) == 1:
        axes = [axes]
    descs = [d for p, d in ABL_ORDER[1:]]
    for ax, key in zip(axes, traces):
        sub = [r for r in rows if r[0] == key and r[2] == 0.01 and r[3] != "SHADE"]
        vals = {r[4]: r[6] for r in sub}
        ys = list(range(len(descs)))
        v = [vals.get(d, 0.0) for d in descs]
        ax.barh(ys, v, color="#2a78d6", height=0.62, edgecolor=SURFACE, linewidth=2)
        for y, x in zip(ys, v):
            ax.annotate(f"{x:+.1f}%", (x, y), xytext=(3 if x >= 0 else -3, 0), textcoords="offset points",
                        va="center", ha="left" if x >= 0 else "right", fontsize=7.5, color=INK)
        ax.set_yticks(ys)
        ax.set_yticklabels(descs, fontsize=8)
        ax.invert_yaxis()
        ax.axvline(0, color=INK2, linewidth=0.8)
        ax.set_title(dict(TRACES)[key], fontsize=9.5, color=INK, loc="left")
        ax.set_xlabel("change in miss ratio vs full SHADE (%), cache 1%", fontsize=8.5, color=INK2)
        style(ax)
        ax.grid(axis="x", color=GRID, linewidth=0.8)
        ax.grid(axis="y", visible=False)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "lab_ablation.png"), dpi=160, facecolor=SURFACE)
    plt.close(fig)


def markdown():
    lines = ["# Cache-lab results (real CDN traces)", "",
             "Generated by `scripts/lab_figures.py` from `results/lab/*.json`. Miss ratios are over the whole",
             "trace (cold start included). Cache size is a fraction of the trace footprint (bytes of all distinct",
             "objects). `*` = offline reference with future knowledge. PFOO-L is an upper bound on any policy's",
             "hit ratio (lower bound on miss ratio).", ""]
    for key, label in TRACES + [DEV]:
        d = load(key + "_main")
        if not d:
            continue
        lines += [f"## {label}", "",
                  f"{d['requests']:,} requests · {d['objects']:,} objects · footprint {d['footprintBytes'] / 1e9:.1f} GB · "
                  f"{100 * d['oneHitObjectFrac']:.1f}% of objects requested once", ""]
        for metric, title in (("objectMissRatio", "Object miss ratio"), ("byteMissRatio", "Byte miss ratio")):
            t = table(d, metric)
            sizes = sorted({r["cacheFrac"] for r in d["results"]})
            bound = {b["cacheFrac"]: b[metric] for b in d["bounds"]}
            lines.append(f"**{title}** (lower is better; best online policy in bold)")
            lines.append("")
            lines.append("| policy | " + " | ".join(f"{100 * s:g}%" for s in sizes) + " |")
            lines.append("|---|" + "---:|" * len(sizes))
            online = [p for p in t if not p.endswith("*")]
            best = {s: min(t[p][s] for p in online) for s in sizes}
            for p in t:
                cells = []
                for s in sizes:
                    v = t[p][s]
                    cells.append(f"**{v:.4f}**" if (v == best[s] and not p.endswith("*")) else f"{v:.4f}")
                lines.append(f"| {p} | " + " | ".join(cells) + " |")
            lines.append("| PFOO-L bound | " + " | ".join(f"{bound[s]:.4f}" for s in sizes) + " |")
            lines.append("")
    rows = improvement_rows()
    if rows:
        lines += ["## SHADE vs the best baseline (object miss ratio)", "",
                  "| trace | cache | SHADE | best baseline | which | change (+ = SHADE fewer misses) | LRU |",
                  "|---|---:|---:|---:|---|---:|---:|"]
        for key, label, s, sh, bb, bname, imp, lru in rows:
            lines.append(f"| {label} | {100 * s:g}% | {sh:.4f} | {bb:.4f} | {bname} | {imp:+.1f}% | {lru:.4f} |")
        lines.append("")
    brows = improvement_rows("byteMissRatio", "SHADE-B")
    if brows:
        lines += ["## SHADE-B (byte objective) vs the best baseline (byte miss ratio)", "",
                  "| trace | cache | SHADE-B | best baseline | which | change (+ = fewer missed bytes) |",
                  "|---|---:|---:|---:|---|---:|"]
        for key, label, s, sh, bb, bname, imp, _ in brows:
            lines.append(f"| {label} | {100 * s:g}% | {sh:.4f} | {bb:.4f} | {bname} | {imp:+.1f}% |")
        lines.append("")
    arows = ablation_rows()
    if arows:
        lines += ["## Ablations (object miss ratio; change vs full SHADE)", "",
                  "| trace | cache | variant | miss ratio | change |", "|---|---:|---|---:|---:|"]
        for key, label, s, pol, desc, v, ch in arows:
            lines.append(f"| {label} | {100 * s:g}% | {desc} | {v:.4f} | {ch:+.1f}% |")
        lines.append("")
    with open(os.path.join(ROOT, "docs", "LAB_RESULTS.md"), "w") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    miss_curves("objectMissRatio", "lab_miss_ratio.png", "object miss ratio (lower is better)")
    miss_curves("byteMissRatio", "lab_byte_miss_ratio.png", "byte miss ratio (lower is better)", shade_name="SHADE-B")
    improvement_figure()
    ablation_figure()
    markdown()
    print("wrote docs/LAB_RESULTS.md and docs/figures/lab_*.png", file=sys.stderr)
