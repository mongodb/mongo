# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Plotting helpers for join cost model calibration."""

import csv
import os

import matplotlib.pyplot as plt
from scipy.stats import pearsonr

# We only want to write these fields to the committed CSV file.
CSV_FIELDS = ["scenario", "join_field", "pred_const", "inlj_time_ms", "hj_time_ms"]


def plot_cost_vs_time(csv_rows: list[dict], output_dir: str):
    """Write calibration results to CSV and generate a cost-vs-time scatter plot."""
    os.makedirs(output_dir, exist_ok=True)

    scenario = csv_rows[0]["scenario"]
    csv_path = os.path.join(output_dir, f"join_times_{scenario}.csv")
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(csv_rows)
    print(f"\nResults written to {csv_path}")

    fig, ax = plt.subplots(figsize=(10, 7))

    algo_configs = [
        ("INLJ", "inlj_time_ms", "inlj_cost", "#e74c3c", "o"),
        ("HJ", "hj_time_ms", "hj_cost", "#3498db", "s"),
    ]

    all_times: list[float] = []
    all_costs: list[float] = []
    correlation_lines: list[str] = []

    # Per-algorithm: scatter plot and Pearson correlation
    for algo, time_col, cost_col, color, marker in algo_configs:
        pairs = [(r[time_col], r[cost_col]) for r in csv_rows if r[time_col] and r[cost_col]]
        times, costs = [p[0] for p in pairs], [p[1] for p in pairs]

        ax.scatter(times, costs, c=color, marker=marker, label=algo, s=20, alpha=0.8)

        r, _ = pearsonr(times, costs)
        correlation_lines.append(f"{algo + ':':8s} r={r:.3f}")

        all_times.extend(times)
        all_costs.extend(costs)

    # Pooled Pearson correlation across both algorithms
    pooled_r, _ = pearsonr(all_times, all_costs)
    correlation_lines.append(f"{'Pooled:':8s} r={pooled_r:.3f}")

    print(f"\n--- Correlation ({scenario}) ---")
    for line in correlation_lines:
        print(f"  {line}")

    annotation = "\n".join(correlation_lines)
    ax.text(
        0.97,
        0.03,
        annotation,
        transform=ax.transAxes,
        ha="right",
        va="bottom",
        fontsize=9,
        family="monospace",
        bbox=dict(boxstyle="round,pad=0.4", facecolor="white", alpha=0.8),
    )

    ax.set_xlabel("Execution Time (ms)")
    ax.set_ylabel("Optimizer Cost Estimate")
    ax.set_title(f"Execution Time vs Cost Estimate: INLJ and HJ ({scenario})")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    png_path = os.path.join(output_dir, f"cost_vs_time_{scenario}.png")
    fig.savefig(png_path, dpi=150)
    print(f"Saved {png_path}")
    plt.show()
