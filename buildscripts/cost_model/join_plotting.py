# Copyright (C) 2026-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Plotting helpers for join cost model calibration."""

import csv
import os

import matplotlib.pyplot as plt


def plot_cost_vs_time(csv_rows: list[dict], output_dir: str):
    """Write calibration results to CSV and generate a cost-vs-time scatter plot."""
    os.makedirs(output_dir, exist_ok=True)

    scenario = csv_rows[0]["scenario"]
    csv_path = os.path.join(output_dir, f"cost_vs_time_{scenario}.csv")
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=csv_rows[0].keys())
        writer.writeheader()
        writer.writerows(csv_rows)
    print(f"\nResults written to {csv_path}")

    fig, ax = plt.subplots(figsize=(10, 7))

    for algo, time_col, cost_col, color, marker in [
        ("INLJ", "inlj_time_ms", "inlj_cost", "#e74c3c", "o"),
        ("HJ", "hj_time_ms", "hj_cost", "#3498db", "s"),
    ]:
        times, costs = [], []
        for r in csv_rows:
            t, c = r[time_col], r[cost_col]
            if t is None or c is None:
                continue
            times.append(float(t))
            costs.append(float(c))

        ax.scatter(times, costs, c=color, marker=marker, label=algo, s=20, alpha=0.8)

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
