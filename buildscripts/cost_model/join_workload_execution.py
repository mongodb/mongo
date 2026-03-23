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
"""Join workload execution helpers for calibration."""

from __future__ import annotations

from typing import NamedTuple

from database_instance import DatabaseInstance


class JoinExplainResult(NamedTuple):
    exec_time_ms: float | None
    used_disk: bool | None
    algorithm: str
    cost_estimate: float


def abbreviate_stage(stage_name: str) -> str:
    """Abbreviate join algorithm stage names."""
    abbreviations = {
        "HASH_JOIN_EMBEDDING": "HJ",
        "NESTED_LOOP_JOIN_EMBEDDING": "NLJ",
        "INDEXED_NESTED_LOOP_JOIN_EMBEDDING": "INLJ",
    }
    return abbreviations.get(stage_name, stage_name)


async def run_join_explain(
    database: DatabaseInstance,
    collection_name: str,
    pipeline: list,
    verbosity: str = "executionStats",
) -> JoinExplainResult:
    explain = await database.explain_aggregate(collection_name, pipeline, verbosity)
    cursor = explain["stages"][0]["$cursor"]

    exec_time_ms, used_disk = None, None
    if verbosity == "executionStats":
        stats = cursor["executionStats"]["executionStages"]
        exec_time_ms = stats["executionTimeNanos"] / 1e6
        used_disk = stats["inputStage"].get("usedDisk")
    query_plan = cursor["queryPlanner"]["winningPlan"]["queryPlan"]

    assert query_plan["leftEmbeddingField"] == "none", f"Expected {collection_name} as outer table"
    return JoinExplainResult(
        exec_time_ms=exec_time_ms,
        used_disk=used_disk,
        algorithm=abbreviate_stage(query_plan["stage"]),
        cost_estimate=query_plan["costEstimate"],
    )
