#!/usr/bin/env python3
"""Render the report / README figures from results/*.json (requires matplotlib).

    python3 scripts/make_figures.py            # -> docs/figures/*.png
"""
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")
OUT = os.path.join(ROOT, "docs", "figures")
os.makedirs(OUT, exist_ok=True)

# Validated categorical palette (same as the dashboard); colour follows the algorithm.
SLOTS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
COLOR = {"pack": SLOTS[0], "lru": SLOTS[1], "lfu": SLOTS[2], "gdsf": SLOTS[3], "greedy-epoch": SLOTS[4],
         "dp-reactive": SLOTS[5], "dp-predictive": SLOTS[6], "dp-oracle": "#52514e", "belady": "#a3a19a"}
INK, INK2, MUTED, GRID = "#0b0b0b", "#52514e", "#6f6e69", "#e1e0d9"

plt.rcParams.update({
    "figure.dpi": 150, "savefig.dpi": 150, "font.size": 9.5, "axes.edgecolor": "#c3c2b7", "axes.labelcolor": INK2,
    "xtick.color": MUTED, "ytick.color": MUTED, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False, "axes.titlesize": 11, "axes.titleweight": "semibold",
    "axes.titlecolor": INK, "legend.frameon": False, "legend.fontsize": 8.5, "figure.facecolor": "#fcfcfb",
    "axes.facecolor": "#fcfcfb", "savefig.facecolor": "#fcfcfb", "lines.linewidth": 2,
})


def load(name):
    with open(os.path.join(RES, name)) as f:
        return json.load(f)


def style(key, oracle=False):
    return dict(color=COLOR.get(key, SLOTS[7]), linestyle="--" if oracle else "-",
                linewidth=2.6 if key == "pack" else 1.8, marker="o", markersize=3.5)


def save(fig, name):
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, name))
    plt.close(fig)
    print("  wrote docs/figures/" + name)


def fig_compare():
    d = load("compare.json")
    pats = [p["pattern"] for p in d["patterns"]]
    keys = [a["key"] for a in d["patterns"][0]["algorithms"]]
    names = {a["key"]: a["name"] for a in d["patterns"][0]["algorithms"]}
    fig, ax = plt.subplots(figsize=(10, 3.8))
    w = 0.8 / len(keys)
    for k_i, k in enumerate(keys):
        vals, errs = [], []
        for p in d["patterns"]:
            a = next(x for x in p["algorithms"] if x["key"] == k)
            vals.append(100 * a["mean"]["benefitRatio"])
            errs.append(100 * a["std"]["benefitRatio"])
        xs = [i + (k_i - len(keys) / 2 + 0.5) * w for i in range(len(pats))]
        oracle = k in ("dp-oracle", "belady")
        ax.bar(xs, vals, w * 0.9, color=COLOR.get(k), alpha=0.45 if oracle else 1.0, label=names[k],
               hatch="//" if oracle else None, edgecolor=COLOR.get(k) if oracle else "none", linewidth=0)
    ax.set_xticks(range(len(pats)))
    ax.set_xticklabels(pats)
    ax.set_ylabel("benefit captured (%)")
    ax.set_title(f"Benefit captured by traffic pattern (mean of {d['seeds']} seeds; hatched = oracle)")
    ax.legend(ncol=5, loc="upper left", bbox_to_anchor=(0, -0.1))
    ax.grid(axis="x", visible=False)
    save(fig, "compare.png")


def fig_sweep(name, pattern, field, title, fname, xfmt=None, logx=False, scale=100, ylabel=None):
    d = load(name + ".json")
    p = next(x for x in d["patterns"] if x["pattern"] == pattern)
    fig, ax = plt.subplots(figsize=(6.2, 3.6))
    for s in p["series"]:
        ys = [None if v is None else v * scale for v in s["mean"][field]]
        ax.plot(d["x"], ys, label=s["name"], **style(s["key"], s["oracle"]))
    if logx:
        ax.set_xscale("log")
    ax.set_xticks(d["x"])
    ax.set_xticklabels([xfmt(x) if xfmt else str(x) for x in d["x"]])
    ax.minorticks_off()
    ax.set_xlabel(d["xLabel"])
    ax.set_ylabel(ylabel or field)
    ax.set_title(title)
    ax.legend(ncol=2, fontsize=7.5)
    save(fig, fname)


def fig_ablation():
    d = load("ablation.json")
    fig, axes = plt.subplots(1, len(d["patterns"]), figsize=(12, 3.4), sharey=True)
    for ax, p in zip(axes, d["patterns"]):
        full = next(a for a in p["algorithms"] if a["key"] == "pack")["mean"]["benefitRatio"]
        rows = [(a["name"], 100 * (a["mean"]["benefitRatio"] - full)) for a in p["algorithms"] if a["key"] != "pack"]
        names = [r[0] for r in rows]
        vals = [r[1] for r in rows]
        ax.barh(names, vals, color="#9a9890", height=0.6)
        ax.axvline(0, color="#c3c2b7", linewidth=1)
        ax.set_title(p["pattern"])
        ax.grid(axis="y", visible=False)
        ax.set_xlabel("Δ benefit vs PACK-DP (pts)")
        ax.invert_yaxis()
    fig.suptitle("Ablation: every variant minus full PACK-DP (left of zero = worse)", fontsize=11, fontweight="semibold")
    save(fig, "ablation.png")


SOLVER_COLOR = {"dp2d": SLOTS[1], "rolling": SLOTS[2], "hirschberg": SLOTS[0], "bnb": SLOTS[6], "fptas:0.1": SLOTS[4],
                "greedy": SLOTS[3], "modgreedy": SLOTS[7], "mitm": SLOTS[5]}
SOLVER_LABEL = {"dp2d": "2-D DP", "rolling": "1-D DP + bit table", "hirschberg": "Hirschberg D&C DP", "bnb": "B&B (MT-U2)",
                "fptas:0.1": "FPTAS ε=0.1", "greedy": "greedy", "modgreedy": "modified greedy", "mitm": "meet-in-the-middle"}


def fig_solvers():
    d = load("solvers.json")
    fig, axes = plt.subplots(1, 3, figsize=(13, 3.8))
    for ax, (block, field, title, ylabel) in zip(axes, [
        ("scalingN", "ms", "Time vs n (C = 8192)", "ms"),
        ("scalingN", "memKB", "Table memory vs n (C = 8192)", "KB"),
        ("hardCorrelated", "ms", "Strongly-correlated instances", "ms"),
    ]):
        b = d[block]
        for a in b["algorithms"]:
            ys = [v if (v is not None and v > 0) else None for v in a[field]]
            if all(v is None for v in ys):
                continue
            ax.plot(b["x"], ys, label=SOLVER_LABEL.get(a["name"], a["name"]), color=SOLVER_COLOR.get(a["name"], INK2),
                    marker="o", markersize=3, linewidth=2.2 if a["name"] == "hirschberg" else 1.6)
        if block != "hardCorrelated":
            ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_title(title)
        ax.set_xlabel("n")
        ax.set_ylabel(ylabel)
        ax.legend(fontsize=7)
    save(fig, "solvers.png")


def fig_optgap():
    d = load("optgap.json")
    fig, ax = plt.subplots(figsize=(6.4, 3.6))
    lab = {"greedy": "greedy (worst instance)", "fptas:0.05": "FPTAS ε=0.05 (worst)", "unitDP-4096": "unit DP, no fill (worst)",
           "unitDP-4096+fill": "unit DP + slack fill (worst)"}
    col = {"greedy": SLOTS[3], "fptas:0.05": SLOTS[4], "unitDP-4096": SLOTS[1], "unitDP-4096+fill": SLOTS[2]}
    xs = [100 * x for x in d["x"]]
    for m in d["methods"]:
        if m["name"] not in lab:
            continue
        ax.plot(xs, [max(v, 1e-3) for v in m["maxGapPct"]], label=lab[m["name"]], color=col[m["name"]], marker="o", markersize=3)
    ax.plot(xs, [max(v, 1e-3) for v in d["boundPct"]], label="bound v_break/OPT (mean)", color=SLOTS[5], linestyle="--")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("cache size (% of catalog bytes)")
    ax.set_ylabel("optimality gap (%)")
    ax.set_title("Price of approximation on real cache instances")
    ax.legend(fontsize=7.5)
    save(fig, "optgap.png")


def fig_timeline():
    d = load("timeline_flash.json")
    fig, ax = plt.subplots(figsize=(10, 3.6))
    for a in d["algorithms"]:
        if a["key"] in ("lfu", "greedy-epoch", "belady"):
            continue
        ys = [100 * v for v in a["epochs"]["benefitRatio"]]
        ax.plot(range(len(ys)), ys, label=a["name"], **{**style(a["key"], a["oracle"]), "marker": None})
    for m in d["markers"]:
        if m["type"] == "flash":
            ax.axvspan(m["epoch"] - 0.4, m["epoch"] + 1.4, color=MUTED, alpha=0.12, linewidth=0)
    ax.set_xlabel("epoch")
    ax.set_ylabel("benefit captured (%)")
    ax.set_title("Flash-crowd traffic, seed 1 (shaded: flash-crowd onset epochs)")
    ax.legend(ncol=6, fontsize=8, loc="lower right")
    save(fig, "timeline_flash.png")


def fig_abr():
    d = load("abr.json")
    fig, axes = plt.subplots(1, 2, figsize=(11, 3.6))
    colors = [SLOTS[1], SLOTS[5], SLOTS[6], SLOTS[0]]
    for ax, (field, title) in zip(axes, [("benefitRatio", "Benefit captured"), ("hitRatio", "Request hit ratio")]):
        for p, c in zip(d["policies"], colors):
            ax.plot([100 * x for x in d["x"]], [100 * v for v in p[field]], label=p["name"], color=c, marker="o", markersize=3,
                    linewidth=2.6 if p["name"].startswith("MCKP") else 1.8)
        ax.set_xscale("log")
        ax.set_xticks([100 * x for x in d["x"]])
        ax.set_xticklabels([f"{100 * x:g}%" for x in d["x"]])
        ax.minorticks_off()
        ax.set_xlabel("cache size (% of all renditions)")
        ax.set_ylabel("%")
        ax.set_title(f"Multi-bitrate video: {title}")
        ax.legend(fontsize=7.5)
    save(fig, "abr.png")


def main():
    if not os.path.isdir(RES):
        sys.exit("results/ not found - run `./core/build/cdnopt experiment all` first")
    fig_compare()
    fig_sweep("capacity", "mixed", "benefitRatio", "Benefit vs cache size (mixed traffic)", "capacity.png",
              xfmt=lambda x: f"{100 * x:g}%", logx=True, ylabel="benefit captured (%)")
    fig_sweep("flash", "flash", "burstHitRatio", "Hit ratio at flash-crowd onsets", "flash.png",
              xfmt=lambda x: f"{100 * x:.0f}%", ylabel="hit ratio during onset epochs (%)")
    fig_sweep("epoch", "drift", "benefitRatio", "Epoch length trade-off (drift traffic)", "epoch.png",
              xfmt=lambda x: f"{x / 1000:g}k", logx=True, ylabel="benefit captured (%)")
    fig_ablation()
    fig_solvers()
    fig_optgap()
    fig_timeline()
    fig_abr()


if __name__ == "__main__":
    main()
