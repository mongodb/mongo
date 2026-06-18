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

import argparse
import asyncio
import os
import random
from collections import Counter
from statistics import mean

from data_generator import DataGenerator
from database_instance import get_database_parameter
from join_calibration_settings import (
    COLLECTION_CARDINALITY,
    join_data_generator,
    join_database,
)
from join_plotting import plot_cost_vs_time
from join_workload_execution import (
    CachedTimes,
    JoinExplainResult,
    load_execution_times,
    run_join_explain,
)
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

    # Restart with WT statistics enabled just to read the leaf page count.
    manager.restart_cold(extra_start_args=["--wiredTigerStatisticsSetting", "all"])
    stats = await manager.database.get_stats("join_coll_1")
    num_leaf_pages = stats["wiredTiger"]["btree"]["row-store leaf pages"]
    mean_per_page_ms = mean_ms / num_leaf_pages
    manager.stop()

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
    cached_times: CachedTimes | None = None,
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

    When cached_times is provided, execution times are looked up from the cache
    instead of being measured. This uses queryPlanner verbosity for all explains, making the run
    much faster for iterating on cost model changes.

    Prints a per-combination summary showing which algorithm is genuinely faster
    and whether the optimizer's majority pick is correct.

    Returns a list of dicts (one per combination) with cost and timing data.
    """
    mode = "cached times" if cached_times else "full execution"
    print(
        f"\n=== Join Algorithm Calibration: {scenario} "
        f"({left_coll} ⨝ {right_coll}, "
        f"cache {cache_size_gb} GB, "
        f"{num_runs} runs per config, {mode}) ==="
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
        f"{'INLJ_cost (L+J=T)':>19} "
        f"{'HJ_cost (L+R+J=T)':>25} {'HJ_spill':>8} "
        f"{'HJ_seqIO':>10} {'INLJ_seqIO':>11} {'INLJ_randIO':>12} {'ML':>6}"
    )
    separator = "-" * len(header)
    print(header)
    print(separator)

    results = []

    async def run_with_join_method(
        join_method: str, pipeline: list, verbosity: str
    ) -> list[JoinExplainResult]:
        if cached_times is None:
            manager.restart_cold(extra_start_args=cache_args)
        await manager.database.set_parameter("internalJoinMethod", join_method)
        run_results = []
        for _ in range(num_runs):
            result = await run_join_explain(manager.database, left_coll, pipeline, verbosity)
            if join_method != "any":
                assert (
                    result.algorithm == join_method
                ), f"Expected {join_method} run, got {result.algorithm}"
            run_results.append(result)
        return run_results

    if cached_times is not None:
        manager.restart_cold(extra_start_args=cache_args)

    def format_join_result_row(
        join_field,
        pred_const,
        optimizer_picks_str,
        inlj_mean,
        hj_mean,
        faster,
        correct,
        inlj_cost_parts,
        hj_cost_parts,
        hj_actual_spilling,
        hj_seq_io_mean,
        inlj_seq_io_mean,
        inlj_rand_io_mean,
        inlj_ml_cases,
    ) -> str:
        def fmt(value, width: int = 10, decimals: int = 1) -> str:
            return f"{value:>{width}.{decimals}f}" if value is not None else f"{'-':>{width}}"

        def fmt_cost_equation(cost_parts, width: int) -> str:
            total, *input_costs = cost_parts
            rounded_inputs = [round(cost) for cost in input_costs if cost is not None]
            total_rounded = round(total)
            join_rounded = total_rounded - sum(rounded_inputs)

            terms = [*rounded_inputs, join_rounded]
            return f"{'+'.join(map(str, terms))}={total_rounded}".rjust(width)

        def fmt_set(values, render=str) -> str:
            return "/".join(render(v) for v in sorted(set(values))) or "-"

        return (
            f"{join_field:<16} {pred_const:<8} {optimizer_picks_str:<24} "
            f"{fmt(inlj_mean)} {fmt(hj_mean)} {faster:>8} {correct:>10} "
            f"{fmt_cost_equation(inlj_cost_parts, 19)} "
            f"{fmt_cost_equation(hj_cost_parts, 25)} "
            f"{fmt_set(hj_actual_spilling, lambda v: 'Y' if v else 'N'):>8} "
            f"{fmt(hj_seq_io_mean)} {fmt(inlj_seq_io_mean, 11)} {fmt(inlj_rand_io_mean, 12)} "
            f"{fmt_set(inlj_ml_cases):>6}"
        )

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

            cached = (
                cached_times.get((scenario, join_field[0], pred_const)) if cached_times else None
            )
            verbosity = "queryPlanner" if cached_times or skip_execution else "executionStats"

            # Using the algorithm which the optimizer picks and forcing INLJ/HJ
            optimizer_results = await run_with_join_method("any", pipeline, verbosity)
            inlj_results = await run_with_join_method("INLJ", pipeline, verbosity)
            hj_results = await run_with_join_method("HJ", pipeline, verbosity)

            algo_freqs = Counter(r.algorithm for r in optimizer_results)
            optimizer_picks_str = " ".join(
                f"{algo} {freq}/{num_runs}" for algo, freq in algo_freqs.most_common()
            )
            majority_algo = algo_freqs.most_common(1)[0][0]

            inlj_times = [r.exec_time_ms for r in inlj_results if r.exec_time_ms is not None]
            hj_times = [r.exec_time_ms for r in hj_results if r.exec_time_ms is not None]

            inlj_cost_mean = mean(r.cost_estimate for r in inlj_results)
            hj_cost_mean = mean(r.cost_estimate for r in hj_results)
            inlj_cost_parts = (
                inlj_cost_mean,
                mean(r.left_input_cost for r in inlj_results),
                None,
            )
            hj_cost_parts = (
                hj_cost_mean,
                mean(r.left_input_cost for r in hj_results),
                mean(r.right_input_cost for r in hj_results),
            )

            hj_actual_spilling = [r.used_disk for r in hj_results if r.used_disk is not None]
            inlj_ml_cases = [r.mackert_lohman_case for r in inlj_results]
            hj_seq_io_mean = mean(r.sequential_io_pages for r in hj_results)
            inlj_seq_io_mean = mean(r.sequential_io_pages for r in inlj_results)
            inlj_rand_io_mean = mean(r.random_io_pages for r in inlj_results)

            inlj_mean = (
                cached.inlj_time_ms
                if cached
                else trim_mean(inlj_times, proportiontocut=TRIMMED_MEAN_PROPORTION)
                if inlj_times
                else None
            )
            hj_mean = (
                cached.hj_time_ms
                if cached
                else trim_mean(hj_times, proportiontocut=TRIMMED_MEAN_PROPORTION)
                if hj_times
                else None
            )

            # Print whether the optimizer is making the correct decision
            faster = "INLJ" if inlj_mean and hj_mean and inlj_mean <= hj_mean else "HJ"
            correct = "✓" if majority_algo == faster else "✗"

            print(
                format_join_result_row(
                    join_field[0],
                    pred_const,
                    optimizer_picks_str,
                    inlj_mean,
                    hj_mean,
                    faster,
                    correct,
                    inlj_cost_parts,
                    hj_cost_parts,
                    hj_actual_spilling,
                    hj_seq_io_mean,
                    inlj_seq_io_mean,
                    inlj_rand_io_mean,
                    inlj_ml_cases,
                )
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
    parser = argparse.ArgumentParser(description="Join Cost Model Calibration")
    parser.add_argument(
        "--join-only",
        action="store_true",
        help="Skip constant calibration (warm scan, CPU, sequential I/O, random I/O) "
        "and only run the join algorithm comparison.",
    )
    parser.add_argument(
        "--execution-times",
        nargs="+",
        metavar="CSV",
        help="CSV file(s) with pre-recorded execution times (from a previous full run). "
        "Only collects fresh cost estimates via queryPlanner explains, skipping actual "
        "query execution and cold restarts.",
    )
    parser.add_argument(
        "--skip-data-generation",
        action="store_true",
        help="Reuse the existing join calibration collections instead of regenerating them.",
    )
    args = parser.parse_args()

    cached_times = None
    if args.execution_times:
        cached_times = load_execution_times(args.execution_times)
        print(f"Loaded {len(cached_times)} cached execution time entries")

    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    repo_root = os.path.abspath(os.path.join(script_directory, "..", ".."))
    mongod_bin = os.path.join(repo_root, "bazel-bin", "install-mongod", "bin", "mongod")
    output_dir = os.path.join(script_directory, "join_output")

    with MongodManager(
        mongod_bin,
        db_config=join_database,
        dbpath=os.path.join(script_directory, "join_calibration_db"),
        extra_args=[
            "--setParameter",
            "internalMeasureQueryExecutionTimeInNanoseconds=true",
            "--setParameter",
            "internalEnableJoinOptimization=true",
            "--setParameter",
            "internalQueryExplainJoinCostComponents=true",
            "--setParameter",
            "featureFlagPathArrayness=true",
            "--setParameter",
            "featureFlagPersistentStats=true",
        ],
    ) as manager:
        if args.skip_data_generation:
            print("\n=== Reusing existing calibration collections (--skip-data-generation) ===")
        else:
            generator = DataGenerator(manager.database, join_data_generator)
            await generator.populate_collections()

        existing_collections = set(await manager.database.database.list_collection_names())
        missing_collections = [
            template.name
            for template in join_data_generator.collection_templates
            if template.name not in existing_collections
        ]
        if missing_collections:
            raise SystemExit(
                f"Missing join calibration collections: {', '.join(missing_collections)}"
            )

        if not args.join_only:
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
        else:
            print("\n=== Skipping constant calibration (--join-only) ===")

        # The ~100MB collections will fit comfortably in the 5GB cache.
        in_cache_results = await calibrate_join_algorithms(
            manager,
            left_coll="join_coll_1",
            right_coll="join_coll_2",
            scenario="in-cache",
            cache_size_gb=5,
            cached_times=cached_times,
        )
        plot_cost_vs_time(in_cache_results, output_dir)

        # The ~300MB collections won't fit in the 256MB cache.
        exceeds_cache_results = await calibrate_join_algorithms(
            manager,
            left_coll="join_coll_1_large",
            right_coll="join_coll_2_large",
            scenario="exceeds-cache",
            cache_size_gb=0.25,
            cached_times=cached_times,
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
