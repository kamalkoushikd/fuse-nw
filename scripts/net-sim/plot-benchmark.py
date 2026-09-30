#!/usr/bin/env python3
"""Generate figures + stats from a net-sim benchmark.csv (see benchmark.sh).

Usage: plot-benchmark.py <benchmark.csv> <output-dir>

Writes fig1..fig7 as both .png (300 DPI, for the wiki/slides) and .pdf
(vector, for LaTeX/Word) into <output-dir>, plus stats.md with the numbers
behind them (means, 95% CIs, coefficient of variation, retransmit
correlation) - the kind of detail a paper's Results section needs beyond
what's in the figures themselves.
"""
import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Okabe-Ito colorblind-safe palette.
COLORS = {"fuse": "#0072B2", "quic": "#D55E00"}
LABELS = {"fuse": "fuse", "quic": "QUIC/HTTP3"}

plt.rcParams.update({
    "figure.dpi": 100,
    "savefig.dpi": 300,
    "font.size": 11,
    "axes.grid": True,
    "grid.alpha": 0.3,
    "axes.spines.top": False,
    "axes.spines.right": False,
})


def load(csv_path):
    data = {"fuse": {"mbps": [], "retx": [], "run": []}, "quic": {"mbps": [], "run": []}}
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            proto = row["protocol"]
            if proto not in data or not row["mbps"] or row["sha_match"] != "1":
                continue
            data[proto]["mbps"].append(float(row["mbps"]))
            data[proto]["run"].append(int(row["run"]))
            if proto == "fuse" and row["retransmits"]:
                data[proto]["retx"].append(int(row["retransmits"]))
    return data


def save(fig, out_dir, name):
    fig.tight_layout()
    fig.savefig(out_dir / f"{name}.png")
    fig.savefig(out_dir / f"{name}.pdf")
    plt.close(fig)


def fig_boxplot(data, out_dir):
    fig, ax = plt.subplots(figsize=(5, 4.5))
    vals = [data["fuse"]["mbps"], data["quic"]["mbps"]]
    bp = ax.boxplot(
        vals, tick_labels=[LABELS["fuse"], LABELS["quic"]], patch_artist=True, showmeans=True,
        meanprops={"marker": "D", "markerfacecolor": "white", "markeredgecolor": "black"},
    )
    for patch, proto in zip(bp["boxes"], ["fuse", "quic"]):
        patch.set_facecolor(COLORS[proto])
        patch.set_alpha(0.6)
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title(f"Throughput distribution, N={len(data['fuse']['mbps'])} runs each")
    save(fig, out_dir, "fig1_boxplot")


def fig_histogram(data, out_dir):
    fig, axes = plt.subplots(1, 2, figsize=(9, 4))
    for ax, proto in zip(axes, ["fuse", "quic"]):
        vals = data[proto]["mbps"]
        ax.hist(vals, bins=20, color=COLORS[proto], alpha=0.75, edgecolor="white")
        ax.axvline(float(np.mean(vals)), color="black", linestyle="--", linewidth=1,
                   label=f"mean={np.mean(vals):.0f}")
        ax.set_xlabel("Throughput (MB/s)")
        ax.set_ylabel("Runs")
        ax.set_title(LABELS[proto])
        ax.legend(fontsize=9)
    fig.suptitle("Throughput distribution by protocol")
    save(fig, out_dir, "fig2_histogram")


def fig_cdf(data, out_dir):
    fig, ax = plt.subplots(figsize=(6, 4.5))
    for proto in ["fuse", "quic"]:
        vals = np.sort(data[proto]["mbps"])
        y = np.arange(1, len(vals) + 1) / len(vals)
        ax.step(vals, y, where="post", color=COLORS[proto], label=LABELS[proto], linewidth=2)
    ax.set_xlabel("Throughput (MB/s)")
    ax.set_ylabel("Empirical CDF")
    ax.set_title("Empirical CDF of per-run throughput")
    ax.legend()
    save(fig, out_dir, "fig3_cdf")


def fig_timeseries(data, out_dir):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax2 = ax.twinx()
    ax.scatter(data["fuse"]["run"], data["fuse"]["mbps"], color=COLORS["fuse"], s=14, alpha=0.8)
    ax2.scatter(data["quic"]["run"], data["quic"]["mbps"], color=COLORS["quic"], s=14, alpha=0.8)
    ax.set_xlabel("Run index")
    ax.set_ylabel("fuse throughput (MB/s)", color=COLORS["fuse"])
    ax2.set_ylabel("QUIC throughput (MB/s)", color=COLORS["quic"])
    ax.tick_params(axis="y", labelcolor=COLORS["fuse"])
    ax2.tick_params(axis="y", labelcolor=COLORS["quic"])
    ax.set_title("Throughput per run, in run order (stability check)")
    save(fig, out_dir, "fig4_timeseries")


def fig_retransmits_hist(data, out_dir):
    fig, ax = plt.subplots(figsize=(5, 4.5))
    vals = data["fuse"]["retx"]
    ax.hist(vals, bins=20, color=COLORS["fuse"], alpha=0.75, edgecolor="white")
    ax.axvline(float(np.mean(vals)), color="black", linestyle="--", linewidth=1,
               label=f"mean={np.mean(vals):.0f}")
    ax.set_xlabel("Retransmits per run")
    ax.set_ylabel("Runs")
    ax.set_title("fuse retransmit count distribution")
    ax.legend()
    save(fig, out_dir, "fig5_retransmits_hist")


def fig_retx_vs_throughput(data, out_dir):
    fig, ax = plt.subplots(figsize=(5.5, 4.5))
    x = np.array(data["fuse"]["retx"], dtype=float)
    y = np.array(data["fuse"]["mbps"], dtype=float)
    ax.scatter(x, y, color=COLORS["fuse"], alpha=0.7, s=20)
    if len(x) > 1:
        r = float(np.corrcoef(x, y)[0, 1])
        m, b = np.polyfit(x, y, 1)
        xs = np.linspace(x.min(), x.max(), 50)
        ax.plot(xs, m * xs + b, color="black", linestyle="--", linewidth=1, label=f"r={r:.2f}")
        ax.legend()
    ax.set_xlabel("Retransmits per run")
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title("fuse: retransmits vs. throughput per run")
    save(fig, out_dir, "fig6_retx_vs_throughput")


def fig_summary_bar(data, out_dir):
    fig, ax = plt.subplots(figsize=(4.5, 4.5))
    protos = ["fuse", "quic"]
    means = [float(np.mean(data[p]["mbps"])) for p in protos]
    stds = [float(np.std(data[p]["mbps"], ddof=1)) for p in protos]
    bars = ax.bar([LABELS[p] for p in protos], means, yerr=stds, capsize=6,
                  color=[COLORS[p] for p in protos], alpha=0.8)
    headroom = max(m + s for m, s in zip(means, stds)) * 0.04
    for bar, m, s in zip(bars, means, stds):
        ax.text(bar.get_x() + bar.get_width() / 2, m + s + headroom, f"{m:.0f}",
                ha="center", va="bottom", fontsize=10)
    ax.margins(y=0.12)
    speedup = means[0] / means[1]
    ax.set_ylabel("Mean throughput (MB/s)")
    ax.set_title(f"fuse is {speedup:.2f}x QUIC's mean throughput")
    save(fig, out_dir, "fig7_summary_bar")


def write_stats(data, out_dir):
    lines = ["# benchmark figure stats\n\n"]
    means = {}
    for proto in ["fuse", "quic"]:
        vals = np.array(data[proto]["mbps"])
        n = len(vals)
        mean = float(vals.mean())
        std = float(vals.std(ddof=1))
        se = std / np.sqrt(n)
        ci = 1.96 * se
        means[proto] = mean
        lines.append(f"## {LABELS[proto]} (N={n})\n\n")
        lines.append(f"- mean: {mean:.1f} MB/s\n")
        lines.append(f"- stdev: {std:.1f} MB/s\n")
        lines.append(f"- 95% CI of the mean: [{mean - ci:.1f}, {mean + ci:.1f}] MB/s\n")
        lines.append(f"- min / median / max: {vals.min():.1f} / {float(np.median(vals)):.1f} / {vals.max():.1f} MB/s\n")
        lines.append(f"- coefficient of variation: {std / mean * 100:.1f}%\n\n")

    lines.append(f"fuse mean / QUIC mean = {means['fuse'] / means['quic']:.3f}x\n\n")

    retx = np.array(data["fuse"]["retx"], dtype=float)
    mbps = np.array(data["fuse"]["mbps"], dtype=float)
    if len(retx) > 1:
        r = float(np.corrcoef(retx, mbps)[0, 1])
        lines.append(
            f"fuse retransmits vs throughput: Pearson r = {r:.3f} "
            f"(mean {retx.mean():.0f}, min {retx.min():.0f}, max {retx.max():.0f})\n"
        )

    text = "".join(lines)
    (out_dir / "stats.md").write_text(text)
    print(text)


def main():
    csv_path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    data = load(csv_path)
    if not data["fuse"]["mbps"] or not data["quic"]["mbps"]:
        sys.exit(f"no usable rows for one or both protocols in {csv_path}")

    fig_boxplot(data, out_dir)
    fig_histogram(data, out_dir)
    fig_cdf(data, out_dir)
    fig_timeseries(data, out_dir)
    fig_retransmits_hist(data, out_dir)
    fig_retx_vs_throughput(data, out_dir)
    fig_summary_bar(data, out_dir)
    write_stats(data, out_dir)
    print(f"figures written to {out_dir}")


if __name__ == "__main__":
    main()
