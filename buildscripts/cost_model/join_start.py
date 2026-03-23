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
"""
Join Cost Model Calibration entry point.

Assumptions:
- The WiredTiger cache is large enough (≥ 100 MB) by default to contain the
  join calibration collection. If the WT cache were too small, the CPU
  measurements would be invalid.
"""

import asyncio
import os
import random

from data_generator import DataGenerator
from database_instance import get_database_parameter
from join_calibration_settings import (
    COLLECTION_CARDINALITY,
    join_data_generator,
    join_database,
)
from join_plotting import plot_cost_vs_time
from join_workload_execution import run_join_explain
from mongod_manager import MongodManager
from scipy.stats import trim_mean

# Removing 10% on each extreme end of the execution times to remove outliers.
TRIMMED_MEAN_PROPORTION = 0.1


async def measure_warm_scan_time(manager: MongodManager, num_runs: int = 30):
    """
    Measures the time of scanning a single tuple in memory.

    After a cold restart, three full collection scans warm the WT cache so all
    data pages are in memory. Subsequent scans incur zero disk I/O.

    The warm scan time is used by 'calibrate_sequential_io' to subtract the CPU
    component from cold scan measurements, isolating the I/O cost.

    Returns the mean warm scan time per tuple in milliseconds.
    """
    print(f"\n=== Warm Scan Measurement ({num_runs} warm scans on join_coll_1) ===")

    manager.restart_cold()
    for _ in range(3):
        await manager.database.explain("join_coll_1", {})
    print("  WT cache warmed (3 full scans)")

    times_ns = []
    for i in range(num_runs):
        explain = await manager.database.explain("join_coll_1", {})
        t = explain["executionStats"]["executionStages"]["executionTimeNanos"]
        times_ns.append(t)
        print(f"  [{i + 1}/{num_runs}] warm scan: {t / 1e6:.3f}ms")

    warm_scan_ns = trim_mean(times_ns, proportiontocut=TRIMMED_MEAN_PROPORTION)
    warm_scan_tuple_ns = warm_scan_ns / COLLECTION_CARDINALITY

    print("\n--- Warm scan summary ---")
    print(f"  Mean warm scan per tuple: {warm_scan_tuple_ns:.3f}ns")

    return warm_scan_tuple_ns / 1e6


async def calibrate_cpu(manager: MongodManager, num_runs: int = 30):
    """
    Measures the CPU cost of processing one tuple by running an in-memory hash
    join, which exercises a broader mix of CPU operations (scan, build, probe)
    than a pure collection scan.

    Both join_coll_1 and join_coll_2 fit in the default WT cache (~100 MB
    each), so the join is purely CPU-bound after warming. We also increase
    the HJ spilling threshold to 200 MB to avoid any disk usage.

    The cost model uses a single CPU factor for all tuple operations, for both
    the processed and outputted documents.

    For a fully in-memory HJ on 'random' (1:1, N = COLLECTION_CARDINALITY):
        Left CollScan:   processed=N,  output=N   -> 2N
        Right CollScan:  processed=N,  output=N   -> 2N
        HJ node:         processed=2N, output=N   -> 3N
        Total tuple operations: 7N

    The cost model will attribute a total cost of (7N * cpuFactor) in terms of
    CPU cost. So, we have to divide the total measured time for this join query
    by 7 to obtain an "average" cpuFactor: cpu_tuple = time_join / (7 * N).

    Returns the per-tuple CPU cost in milliseconds.
    """
    print(f"\n=== CPU Calibration ({num_runs} in-memory HJ runs on random) ===")

    pipeline = [
        {
            "$lookup": {
                "from": "join_coll_2",
                "localField": "random",
                "foreignField": "random",
                "as": "joined",
            }
        },
        {"$unwind": "$joined"},
        {"$count": "total"},
    ]

    manager.restart_cold()
    async with (
        get_database_parameter(
            manager.database,
            "internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill",
        ) as spill_param,
        get_database_parameter(manager.database, "internalJoinMethod") as algo_param,
    ):
        await spill_param.set(200 * 1024 * 1024)
        await algo_param.set("HJ")

        for _ in range(3):
            await run_join_explain(manager.database, "join_coll_1", pipeline)
        print("  WT cache warmed (3 HJ warmup runs)")

        join_times_ms = []
        for i in range(num_runs):
            result = await run_join_explain(manager.database, "join_coll_1", pipeline)
            assert result.algorithm == "HJ", f"Expected HJ but got {result.algorithm}"
            assert not result.used_disk, "HJ spilled to disk during CPU calibration"

            join_times_ms.append(result.exec_time_ms)
            print(f"  [{i + 1}/{num_runs}] in-memory HJ: {result.exec_time_ms:.3f}ms")

    mean_join_ms = trim_mean(join_times_ms, proportiontocut=TRIMMED_MEAN_PROPORTION)
    cpu_tuple_ms = mean_join_ms / (7 * COLLECTION_CARDINALITY)

    print("\n--- CPU summary ---")
    print(f"  Mean in-memory HJ: {mean_join_ms:.3f}ms")
    print(f"  time_tuple: {(cpu_tuple_ms * 1e6):.1f}ns ({cpu_tuple_ms:.6f}ms)")

    return cpu_tuple_ms


async def calibrate_sequential_io(
    manager: MongodManager,
    warm_scan_ms: float,
    num_runs: int = 50,
):
    """
    Measures the cost of sequentially reading one WT leaf page from disk.

    We do a "cold restart" (stop mongod, flush OS cache, restart mongod) between every run.
    That empties both the WT cache and the OS page cache, so the subsequent full collection
    scan reads every page from disk.

    Because a cold scan includes CPU work as well as I/O, we subtract the previously
    measured warm-scan time (warm_scan_ms), which should capture the pure CPU component,
    from each cold-scan measurement. This avoids double-counting CPU work.

    Returns the per-leaf-page sequential I/O cost in milliseconds.
    """
    print(f"\n=== Sequential I/O ({num_runs} cold scans on join_coll_1) ===")

    times_ns = []
    for i in range(num_runs):
        manager.restart_cold()
        explain = await manager.database.explain("join_coll_1", {})
        t = explain["executionStats"]["executionStages"]["executionTimeNanos"]
        times_ns.append(t - warm_scan_ms * 1e6)
        print(f"  [{i + 1}/{num_runs}] cold scan: {t / 1e6:.3f}ms")

    mean_ms = trim_mean(times_ns, proportiontocut=TRIMMED_MEAN_PROPORTION) / 1e6
    stats = await manager.database.get_stats("join_coll_1")
    num_leaf_pages = stats["wiredTiger"]["btree"]["row-store leaf pages"]
    mean_per_page_ms = mean_ms / num_leaf_pages

    print("\n--- Sequential I/O summary ---")
    print(f"  Mean cold scan: {mean_ms:.3f}ms")
    print(f"  Leaf pages: {num_leaf_pages}")
    print(f"  Mean per leaf page: {mean_per_page_ms:.5f}ms")

    return mean_per_page_ms


async def calibrate_random_io(manager: MongodManager, num_lookups: int = 100):
    """
    Measures the cost of one random data-page read (the FETCH stage after an index lookup).

    We do a single cold restart followed by one warmup query which loads the index B-tree
    internal structure into the WT cache. Before each subsequent lookup, the OS cache is
    flushed to avoid nearby data pages being cached (e.g., by read-ahead policies).

    We report the FETCH time (total - IXSCAN) because that is the actual random I/O time
    required for retrieving a data page. We don't subtract any double-counted CPU cost
    here, because it's negligible for a single tuple. Note that we're also not taking
    B-tree height into account yet (see SERVER-117523).

    Returns the per-page random I/O cost in milliseconds.
    """
    print(f"\n=== Random I/O ({num_lookups} cold lookups on join_coll_2) ===")

    # Space values evenly across the key range so no two values land on the same WT data page
    step = COLLECTION_CARDINALITY // (num_lookups + 1)
    population = range(1, COLLECTION_CARDINALITY + 1, step)
    msg = f"Not enough evenly-spaced keys: need {num_lookups + 1}, have {len(population)}"
    assert len(population) >= num_lookups + 1, msg
    values = random.sample(population, num_lookups + 1)

    manager.restart_cold()
    await manager.database.explain("join_coll_2", {"filter": {"unique": values[0]}})
    print("  Warmup done (B-tree structure cached in WT)")

    times_ns = []
    for i, val in enumerate(values[1:]):
        manager.flush_os_cache()

        explain = await manager.database.explain("join_coll_2", {"filter": {"unique": val}})
        stages = explain["executionStats"]["executionStages"]
        total_ns = stages["executionTimeNanos"]
        ix_ns = stages["inputStage"]["executionTimeNanos"]
        fetch_ns = total_ns - ix_ns

        times_ns.append(fetch_ns)
        print(
            f"  [{i + 1}/{num_lookups}] unique={val}  "
            f"total={total_ns / 1e6:.3f}ms  ix={ix_ns / 1e6:.3f}ms  fetch={fetch_ns / 1e6:.3f}ms"
        )

    fetch_mean_ms = trim_mean(times_ns, proportiontocut=TRIMMED_MEAN_PROPORTION) / 1e6

    print("\n--- Random I/O summary ---")
    print(f"  FETCH mean: {fetch_mean_ms:.3f}ms")

    return fetch_mean_ms


async def calibrate_join_algorithms(
    manager: MongodManager,
    left_coll: str,
    right_coll: str,
    scenario: str,
    cache_size_gb: float,
    num_runs: int = 10,
):
    """
    Compares INLJ vs HJ by running $lookup queries across join fields and selectivities.

    The caller chooses which collections and WT cache size to use, enabling two
    scenarios: one where the data exceeds the cache and one where the data is fully
    cache-resident.

    For each (join_field, predicate) combination we run three rounds, each after a
    cold restart so cache conditions are comparable:
      1. Any (algorithm chosen by optimizer)
      2. Force INLJ
      3. Force HJ

    Combinations whose estimated output cardinality exceeds the 500K threshold are too expensive
    to execute. For those we only collect the optimizer's algorithm pick and verify that HJ was
    chosen, which is the best algorithm for these cases.

    Prints a per-combination summary showing which algorithm is genuinely faster
    and whether the optimizer's majority pick is correct.

    Returns a list of dicts (one per combination) with cost and timing data.
    """
    print(
        f"\n=== Join Algorithm Calibration: {scenario} "
        f"({left_coll} ⨝ {right_coll}, "
        f"cache {cache_size_gb} GB, "
        f"{num_runs} runs per config) ==="
    )

    cache_args = ["--wiredTigerCacheSizeGB", str(cache_size_gb)]
    join_fields = [
        # (name, number of distinct values)
        ("unique", COLLECTION_CARDINALITY),
        ("uniform_64k", 65536),
        ("uniform_4k", 4096),
        ("uniform_256", 256),
        ("uniform_16", 16),
    ]
    predicate_constants = [4, 16, 64, 256, 1024, 4096, 16384, 65536]

    header = (
        f"{'join_field':<16} {'pred':<8} {'optimizer_picks':<24} "
        f"{'INLJ_ms':>10} {'HJ_ms':>10} {'faster':>8} {'correct':>10} "
        f"{'INLJ_cost':>12} {'HJ_cost':>12}"
    )
    separator = "-" * len(header)
    print(header)
    print(separator)

    results = []

    for join_field in join_fields:
        for pred_const in predicate_constants:
            pipeline = [
                {"$match": {"random": {"$lte": pred_const}}},
                {
                    "$lookup": {
                        "from": right_coll,
                        "localField": join_field[0],
                        "foreignField": join_field[0],
                        "as": "joined",
                    }
                },
                {"$unwind": "$joined"},
                {"$count": "total"},
            ]
            estimated_output = pred_const * (COLLECTION_CARDINALITY / join_field[1])
            skip_execution = estimated_output > 500_000
            verbosity = "queryPlanner" if skip_execution else "executionStats"

            # Using the algorithm which the optimizer picks
            manager.restart_cold(extra_start_args=cache_args)
            await manager.database.set_parameter("internalJoinMethod", "any")
            algo_freqs: dict[str, int] = {}
            for _ in range(num_runs):
                result = await run_join_explain(manager.database, left_coll, pipeline, verbosity)
                algo_freqs[result.algorithm] = algo_freqs.get(result.algorithm, 0) + 1

            optimizer_picks_str = " ".join(
                f"{algo} {freq}/{num_runs}"
                for algo, freq in sorted(algo_freqs.items(), key=lambda x: -x[1])
            )
            majority_algo = max(algo_freqs, key=algo_freqs.get)

            inlj_mean, hj_mean = None, None
            inlj_cost_mean, hj_cost_mean = None, None

            # Forcing INLJ
            manager.restart_cold(extra_start_args=cache_args)
            await manager.database.set_parameter("internalJoinMethod", "INLJ")

            inlj_times, inlj_costs = [], []
            for _ in range(num_runs):
                result = await run_join_explain(manager.database, left_coll, pipeline, verbosity)
                assert result.algorithm == "INLJ", f"Expected INLJ but got {result.algorithm}"
                inlj_costs.append(result.cost_estimate)
                if result.exec_time_ms is not None:
                    inlj_times.append(result.exec_time_ms)

            # Forcing HJ
            manager.restart_cold(extra_start_args=cache_args)
            await manager.database.set_parameter("internalJoinMethod", "HJ")

            hj_times, hj_costs = [], []
            for _ in range(num_runs):
                result = await run_join_explain(manager.database, left_coll, pipeline, verbosity)
                assert result.algorithm == "HJ", f"Expected HJ but got {result.algorithm}"
                hj_costs.append(result.cost_estimate)
                if result.exec_time_ms is not None:
                    hj_times.append(result.exec_time_ms)

            inlj_cost_mean = sum(inlj_costs) / len(inlj_costs)
            hj_cost_mean = sum(hj_costs) / len(hj_costs)
            if inlj_times:
                inlj_mean = trim_mean(inlj_times, proportiontocut=TRIMMED_MEAN_PROPORTION)
            if hj_times:
                hj_mean = trim_mean(hj_times, proportiontocut=TRIMMED_MEAN_PROPORTION)

            # Print whether the optimizer is making the correct decision
            faster = "HJ" if skip_execution or hj_mean < inlj_mean else "INLJ"
            correct = "✓" if majority_algo == faster else "✗"

            def fmt(v, width=10, decimals=1):
                return f"{v:>{width}.{decimals}f}" if v is not None else f"{'-':>{width}}"

            print(
                f"{join_field[0]:<16} {pred_const:<8} {optimizer_picks_str:<24} "
                f"{fmt(inlj_mean)} {fmt(hj_mean)} {faster:>8} {correct:>10} "
                f"{fmt(inlj_cost_mean, 12, 2)} {fmt(hj_cost_mean, 12, 2)}"
            )

            results.append(
                {
                    "scenario": scenario,
                    "join_field": join_field[0],
                    "pred_const": pred_const,
                    "skipped": skip_execution,
                    "optimizer_majority": majority_algo,
                    "inlj_time_ms": inlj_mean,
                    "hj_time_ms": hj_mean,
                    "inlj_cost": inlj_cost_mean,
                    "hj_cost": hj_cost_mean,
                    "faster": faster,
                    "correct": correct,
                }
            )

    await manager.database.set_parameter("internalJoinMethod", "any")
    print(separator)
    return results


async def main():
    """Entry point function."""
    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    repo_root = os.path.abspath(os.path.join(script_directory, "..", ".."))
    mongod_bin = os.path.join(repo_root, "bazel-bin", "install-mongod", "bin", "mongod")
    output_dir = os.path.join(script_directory, "join_output")

    with MongodManager(
        mongod_bin,
        db_config=join_database,
        dbpath="~/mongo/join_calibration_db",
        extra_args=[
            # To be able to retrieve the WT "row-store leaf pages" statistic
            "--wiredTigerStatisticsSetting",
            "all",
            "--setParameter",
            "internalMeasureQueryExecutionTimeInNanoseconds=true",
            "--setParameter",
            "internalEnableJoinOptimization=true",
        ],
    ) as manager:
        generator = DataGenerator(manager.database, join_data_generator)
        await generator.populate_collections()

        warm_scan_tuple_ms = await measure_warm_scan_time(manager)
        cpu_tuple_ms = await calibrate_cpu(manager)
        time_seq_page_ms = await calibrate_sequential_io(
            manager, warm_scan_ms=warm_scan_tuple_ms * COLLECTION_CARDINALITY
        )
        time_rand_page_ms = await calibrate_random_io(manager)

        print("\n=== Cost Coefficient Ratios ===")
        print(f"  cpuFactor    = 1.0 ({cpu_tuple_ms:.6f}ms)")
        print(
            f"  seqIOFactor  = {time_seq_page_ms / cpu_tuple_ms:.1f}"
            f"  ({time_seq_page_ms:.4f}ms / {cpu_tuple_ms:.6f}ms)"
        )
        print(
            f"  randIOFactor = {time_rand_page_ms / cpu_tuple_ms:.1f}"
            f"  ({time_rand_page_ms:.4f}ms / {cpu_tuple_ms:.6f}ms)"
        )

        # The ~100MB collections will fit comfortably in the 5GB cache.
        in_cache_results = await calibrate_join_algorithms(
            manager,
            left_coll="join_coll_1",
            right_coll="join_coll_2",
            scenario="in-cache",
            cache_size_gb=5,
        )
        plot_cost_vs_time(in_cache_results, output_dir)

        # The ~300MB collections won't fit in the 256MB cache.
        exceeds_cache_results = await calibrate_join_algorithms(
            manager,
            left_coll="join_coll_1_large",
            right_coll="join_coll_2_large",
            scenario="exceeds-cache",
            cache_size_gb=0.25,
        )
        plot_cost_vs_time(exceeds_cache_results, output_dir)

    print("DONE!")


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
