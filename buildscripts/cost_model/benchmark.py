# Copyright (C) 2022-present MongoDB, Inc.
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
"""A/B performance test to compare plans produced by different sets of cost model coefficients."""

from __future__ import annotations

import asyncio
import logging
from dataclasses import asdict, dataclass
from typing import Sequence

import bson.json_util as json
import execution_tree
import physical_tree
from config import BenchmarkConfig
from database_instance import DatabaseInstance, Pipeline, get_database_parameter
from scipy import stats


@dataclass
class CostModelCoefficients:
    """Represent MongoDB's Cost Model Coefficients initially defined in the cost_model.idl file."""

    scan_incremental_cost: float | None = None
    scan_startup_cost: float | None = None
    index_scan_incremental_cost: float | None = None
    index_scan_startup_cost: float | None = None
    seek_cost: float | None = None
    seek_startup_cost: float | None = None
    filter_incremental_cost: float | None = None
    filter_startup_cost: float | None = None
    eval_incremental_cost: float | None = None
    eval_startup_cost: float | None = None
    group_by_incremental_cost: float | None = None
    group_by_startup_cost: float | None = None
    unwind_incremental_cost: float | None = None
    unwind_startup_cost: float | None = None
    nested_loop_join_incremental_cost: float | None = None
    nested_loop_join_startup_cost: float | None = None
    hash_join_incremental_cost: float | None = None
    hash_join_startup_cost: float | None = None
    merge_join_incremental_cost: float | None = None
    merge_join_startup_cost: float | None = None
    unique_incremental_cost: float | None = None
    unique_startup_cost: float | None = None
    collation_incremental_cost: float | None = None
    collation_startup_cost: float | None = None
    collation_with_limit_incremental_cost: float | None = None
    collation_with_limit_startup_cost: float | None = None
    union_incremental_cost: float | None = None
    union_startup_cost: float | None = None
    exchange_incremental_cost: float | None = None
    exchange_startup_cost: float | None = None
    limit_skip_incremental_cost: float | None = None
    limit_skip_startup_cost: float | None = None

    def to_dict(self):
        """Convert to a dictionary with keys defined according to cost_model.idl."""
        fields = asdict(self)
        return {to_camel_case(k): v for k, v in fields.items() if v is not None}


def to_camel_case(string):
    """Convert a snake_case string to camelCase one."""
    words = string.split("_")
    return words[0] + "".join(w.capitalize() for w in words[1:])


@dataclass
class BenchmarkTask:
    """Define an A/B performance testing task."""

    collection_name: str
    pipeline: Pipeline

    # Statistical significance threshold, usually 0.05 or 0.05.
    threshold: float

    # Cost Model Coefficients overrides of Variant A.
    cost_model_a: CostModelCoefficients

    # Cost Model Coefficients overrides of Variant B.
    cost_model_b: CostModelCoefficients

    def print(self):
        """Prints the task."""
        print("Cost Model Coefficients Overrides:")
        print(f"\tA: {self.cost_model_a.to_dict()}")
        print(f"\tB: {self.cost_model_b.to_dict()}")
        print(f"threshold: {self.threshold}")
        print(f"collection: {self.collection_name}")
        print(f"pipeline: {self.pipeline}")


@dataclass
class BenchmarkResult:
    """Represent the A/B performance testing results."""

    task: BenchmarkTask
    variant_a: ExperimentResult
    variant_b: ExperimentResult

    # If the p-value is less than the task threshold value,
    # there is no significant difference between the variant of the Cost Model Coefficients.
    pvalue: float

    def print(self):
        """Print the results."""
        print("## Benchmark Task")
        self.task.print()
        print("\n## Result")
        print(f"Means: A: {self.variant_a.mean:,.2f}, B: {self.variant_b.mean:,.2f}.")
        print(f"t-test's p-value: {self.pvalue}.")
        if self.pvalue < self.task.threshold:
            print("The means are significantly different.")
        else:
            print("The means are not significantly different.")

        print("\n### A\n")
        self.variant_a.print()

        print("\n### B\n")
        self.variant_b.print()


@dataclass
class ExperimentResult:
    """Represent one variant results of an A/B performance test."""

    explain: Sequence[dict[str, any]]
    physical_tree: Sequence[physical_tree.Node]
    execution_tree: Sequence[execution_tree.Node]
    mean: float

    def print(self, index: int = None):
        """Print the results."""
        if index is None:
            index = len(self.explain) // 2

        print("ABT Physical Tree")
        self.physical_tree[index].print()
        print("\nSBE Execution Tree")
        self.execution_tree[index].print()


async def benchmark(config: BenchmarkConfig, database: DatabaseInstance, task: BenchmarkTask):
    """Run the A/B performance task.

    It executes the given pipeline for both overrides of Cost Model Coefficients,
    then runs Student's t-test to prove a Null hypothesis that the means of the execution times
    of both overrides are equal. If the p-value produced by the t-test is less than the threshold
    value (usually 0.05 or 0.01) we can say that the Null hypothesis is proven and there is
    no significant difference in the execution times.
    """
    async with get_database_parameter(database, "internalCostModelCoefficients") as db_param:
        await db_param.set(json.dumps(task.cost_model_a.to_dict()))
        result_a = await run(config, database, task.collection_name, task.pipeline)

        await db_param.set(json.dumps(task.cost_model_b.to_dict()))
        result_b = await run(config, database, task.collection_name, task.pipeline)

    variant_a = make_variant(result_a)
    variant_b = make_variant(result_b)

    execution_times_a = [et.total_execution_time for et in variant_a.execution_tree]
    execution_times_b = [et.total_execution_time for et in variant_b.execution_tree]

    ttest_result = stats.ttest_ind(execution_times_a, execution_times_b, equal_var=False)

    return BenchmarkResult(
        task=task, variant_a=variant_a, variant_b=variant_b, pvalue=ttest_result.pvalue
    )


def make_variant(explain: Sequence[dict[str, any]]) -> ExperimentResult:
    """Make one variant of the A/B test."""
    pt = [physical_tree.build(e["queryPlanner"]["winningPlan"]["queryPlan"]) for e in explain]
    et = [execution_tree.build_execution_tree(e["executionStats"]) for e in explain]
    mean = sum(et.total_execution_time for et in et) / len(et)
    return ExperimentResult(explain=explain, physical_tree=pt, execution_tree=et, mean=mean)


async def run(
    config: BenchmarkConfig, database: DatabaseInstance, collection: str, pipeline: Pipeline
):
    """Run one variant of the A/B test."""

    # warmup
    for _ in range(config.warmup_runs):
        await database.explain(collection, pipeline)

    result = []
    for _ in range(config.runs):
        explain = await database.explain(collection, pipeline)
        if explain["ok"] == 1:
            result.append(explain)
        else:
            logging.warn("Query execution failed: %s", explain)
    return result


async def smoke_test():
    """Smoke test and usage example function."""
    from calibration_settings import main_config

    config = BenchmarkConfig(warmup_runs=3, runs=20)
    database = DatabaseInstance(main_config.database)
    await database.enable_cascades(True)
    cost_model_a = CostModelCoefficients(index_scan_incremental_cost=0.0001)
    cost_model_b = CostModelCoefficients(index_scan_incremental_cost=0.9)
    task = BenchmarkTask(
        collection_name="c_str_05_45000",
        pipeline=[{"$match": {"choice1": "hello", "choice2": "gaussian"}}],
        cost_model_a=cost_model_a,
        cost_model_b=cost_model_b,
        threshold=0.05,
    )

    res = await benchmark(config, database, task)
    res.print()


if __name__ == "__main__":
    loop = asyncio.get_event_loop()
    loop.run_until_complete(smoke_test())
