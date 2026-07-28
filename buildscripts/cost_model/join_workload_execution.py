# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Join workload execution helpers for calibration."""

from __future__ import annotations

import csv
from typing import NamedTuple

from database_instance import DatabaseInstance


class JoinExplainResult(NamedTuple):
    exec_time_ms: float | None
    used_disk: bool | None
    algorithm: str
    cost_estimate: float
    left_input_cost: float
    right_input_cost: float | None  # None for INLJ
    mackert_lohman_case: int | None
    sequential_io_pages: float | None
    random_io_pages: float | None


class CachedExecutionTime(NamedTuple):
    inlj_time_ms: float | None
    hj_time_ms: float | None


CachedTimes = dict[tuple[str, str, int], CachedExecutionTime]

MACKERT_LOHMAN_CASES = {
    "collection-fits-cache": 1,
    "returned-docs-fit-cache": 2,
    "partial-eviction": 3,
}


def load_execution_times(csv_paths: list[str]) -> CachedTimes:
    """
    Load pre-recorded execution times from CSV files.
    Returns a dict keyed by (scenario, join_field, pred_const).
    """
    cached: CachedTimes = {}
    for path in csv_paths:
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                key = (row["scenario"], row["join_field"], int(row["pred_const"]))
                cached[key] = CachedExecutionTime(
                    inlj_time_ms=float(row["inlj_time_ms"]) if row["inlj_time_ms"] else None,
                    hj_time_ms=float(row["hj_time_ms"]) if row["hj_time_ms"] else None,
                )
    return cached


JOIN_STAGE_ABBREVIATIONS = {
    "HASH_JOIN_EMBEDDING": "HJ",
    "NESTED_LOOP_JOIN_EMBEDDING": "NLJ",
    "INDEXED_NESTED_LOOP_JOIN_EMBEDDING": "INLJ",
}


def abbreviate_stage(stage_name: str) -> str:
    """Abbreviate join algorithm stage names."""
    return JOIN_STAGE_ABBREVIATIONS.get(stage_name, stage_name)


async def run_join_explain(
    database: DatabaseInstance,
    collection_name: str,
    pipeline: list,
    verbosity: str = "executionStats",
) -> JoinExplainResult:
    explain = await database.explain_aggregate(collection_name, pipeline, verbosity)
    container = explain["stages"][0]["$cursor"] if "stages" in explain else explain
    join_node = container["queryPlanner"]["winningPlan"]["queryPlan"]
    while join_node["stage"] not in JOIN_STAGE_ABBREVIATIONS:
        join_node = join_node["inputStage"]

    exec_time_ms, used_disk = None, None
    if verbosity == "executionStats":
        # Walk down to the topmost SBE stage of the join subtree, so that the measurements
        # exclude the pushed-down suffix stages (and their spill stats) above the join.
        stats = container["executionStats"]["executionStages"]
        while stats["planNodeId"] != join_node["planNodeId"]:
            stats = stats["inputStage"]
        exec_time_ms = stats["executionTimeNanos"] / 1e6
        used_disk = stats["inputStage"].get("usedDisk")
    join_cost_components = join_node.get("joinCostComponents", {})

    assert join_node["leftEmbeddingField"] == "none", f"Expected {collection_name} as outer table"

    return JoinExplainResult(
        exec_time_ms=exec_time_ms,
        used_disk=used_disk,
        algorithm=abbreviate_stage(join_node["stage"]),
        cost_estimate=join_node["costEstimate"],
        left_input_cost=join_node["inputStages"][0]["costEstimate"],
        right_input_cost=join_node["inputStages"][1].get("costEstimate"),
        mackert_lohman_case=MACKERT_LOHMAN_CASES.get(join_cost_components.get("mackertLohmanCase")),
        sequential_io_pages=join_cost_components.get("sequentialIOPages"),
        random_io_pages=join_cost_components.get("randomIOPages"),
    )
