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
"""
End2End testing.

The test executes the given query pipelines with the given Cost Model Coefficients and compares
the predicted cost of every ABT node with the actual running time of the nodes.
It produces descriptive statistics (mean, stddev, min, max) and calculates R2 to
estimate quality of the tested Cost Model.
"""

import asyncio
import dataclasses
import os
from typing import Callable, Sequence, Tuple

import config
import execution_tree as et
import experiment as exp
import numpy as np
import pandas as pd
import physical_tree as pt
import workload_execution
from benchmark import CostModelCoefficients
from calibration_settings import HIDDEN_STRING_VALUE, distributions, main_config
from cost_estimator import ExecutionStats
from data_generator import CollectionInfo, DataGenerator
from database_instance import DatabaseInstance, get_database_parameter
from parameters_extractor import get_excution_stats
from random_generator import RandomDistribution
from sklearn.metrics import r2_score
from workload_execution import Query


class CostEstimator:
    """Estimates execution cost of ABT nodes."""

    def __init__(self, cost_model: CostModelCoefficients):
        """Initialize cost estimator."""

        self.cost_model = cost_model

        self.estimators = {
            "PhysicalScan": self.physical_scan,
            "IndexScan": self.index_scan,
            "Seek": self.seek,
            "Filter": self.filter,
            "Evaluation": self.evaluation,
            "GroupBy": self.group_by,
            "Unwind": self.unwind,
            "NestedLoopJoin": self.nested_loop_join,
            "HashJoin": self.hash_join,
            "MergeJoin": self.merge_join,
            "Unique": self.unique,
            "Union": self.union,
            "LimitSkip": self.limit_skip,
            "Root": self.root,
        }

    def estimate(self, abt_node_name: str, cardinality: int) -> float:
        """Estimate ABT node cost."""
        estimator = self.estimators.get(abt_node_name, self.default_estimator)
        return estimator(cardinality)

    def physical_scan(self, cardinality: int) -> float:
        """Estinamate PhysicalScan ABT node."""
        return (
            self.cost_model.scan_startup_cost + cardinality * self.cost_model.scan_incremental_cost
        )

    def index_scan(self, cardinality: int) -> float:
        """Estinamate IndexScan ABT node."""
        return (
            self.cost_model.index_scan_startup_cost
            + cardinality * self.cost_model.index_scan_incremental_cost
        )

    def seek(self, cardinality: int) -> float:
        """Estinamate Seek ABT node."""
        return self.cost_model.seek_startup_cost + cardinality * self.cost_model.seek_cost

    def filter(self, cardinality: int) -> float:
        """Estinamate Filter ABT node."""
        return (
            self.cost_model.filter_startup_cost
            + cardinality * self.cost_model.filter_incremental_cost
        )

    def evaluation(self, cardinality: int) -> float:
        """Estinamate Evaluation ABT node."""
        return (
            self.cost_model.eval_startup_cost + cardinality * self.cost_model.eval_incremental_cost
        )

    def group_by(self, cardinality: int) -> float:
        """Estinamate GroupBy ABT node."""
        return (
            self.cost_model.group_by_startup_cost
            + cardinality * self.cost_model.group_by_incremental_cost
        )

    def unwind(self, cardinality: int) -> float:
        """Estinamate Unwind ABT node."""
        return (
            self.cost_model.unwind_startup_cost
            + cardinality * self.cost_model.unwind_incremental_cost
        )

    def nested_loop_join(self, cardinality: int) -> float:
        """Estinamate NestedLoopJoin ABT node."""
        return (
            self.cost_model.nested_loop_join_startup_cost
            + cardinality * self.cost_model.nested_loop_join_incremental_cost
        )

    def hash_join(self, cardinality: int) -> float:
        """Estinamate HashJoin ABT node."""
        return (
            self.cost_model.hash_join_startup_cost
            + cardinality * self.cost_model.hash_join_incremental_cost
        )

    def merge_join(self, cardinality: int) -> float:
        """Estinamate MergeJoin ABT node."""
        return (
            self.cost_model.merge_join_startup_cost
            + cardinality * self.cost_model.merge_join_incremental_cost
        )

    def unique(self, cardinality: int) -> float:
        """Estinamate Unique ABT node."""
        return (
            self.cost_model.unique_startup_cost
            + cardinality * self.cost_model.unique_incremental_cost
        )

    def union(self, cardinality: int) -> float:
        """Estinamate Union ABT node."""
        return (
            self.cost_model.union_startup_cost
            + cardinality * self.cost_model.union_incremental_cost
        )

    def limit_skip(self, cardinality: int) -> float:
        """Estinamate LimitSkip ABT node."""
        return (
            self.cost_model.limit_skip_startup_cost
            + cardinality * self.cost_model.limit_skip_incremental_cost
        )

    def root(self, _: int) -> float:
        """Root ABT node is always 0."""
        return 0.0

    def default_estimator(self, _: int) -> float:
        """Used if no ABT nodes matched."""
        return -1e10


class AbtCostEstimator:
    """Calculates a cost for the given ABT tree."""

    def __init__(self, estimate_node: Callable[[str, int], float]):
        self.estimate_node = estimate_node

    def estimate(
        self,
        abt: pt.Node,
        sbe: et.Node,
        estimations: Sequence[Tuple[str, ExecutionStats, float]],
        level=0,
    ):
        stats = get_excution_stats(sbe, abt.plan_node_id)
        local_cost = self.estimate_node(abt.node_type, stats.n_processed)
        estimations.append((abt.node_type, stats, local_cost))
        child_cost = sum(
            (self.estimate(child, sbe, estimations, level + 1) for child in abt.children), start=0.0
        )
        return local_cost + child_cost


@dataclasses.dataclass(init=False)
class EndToEndStatisticsRow:
    """Represents a row with descriptive statistics of one query execution."""

    def __init__(
        self,
        pipeline: str = None,
        abt_type: str = None,
        abt_type_id: int = 0,
        execution_time: float = 0.0,
        estimated_cost: float = 0.0,
        n_documents: int = 0,
    ):
        self.pipeline = pipeline if pipeline is not None else ""
        self.abt_type = abt_type if abt_type is not None else ""
        self.abt_type_id = abt_type_id
        self.execution_time = execution_time
        self.estimated_cost = estimated_cost
        self.estimation_error = execution_time - estimated_cost
        self.estimation_error_per_doc = (
            self.estimation_error / n_documents if n_documents != 0 else 0
        )
        self.relative_error = (
            self.estimation_error / self.execution_time if self.execution_time != 0 else 0
        )

    pipeline: str
    abt_type: str
    abt_type_id: int
    execution_time: float
    estimated_cost: float
    estimation_error: float
    estimation_error_per_doc: float
    relative_error: float


def make_config():
    def create_end2end_collection_template(
        name: str, cardinality: int
    ) -> config.CollectionTemplate:
        values = [
            "iqtbr5b5is",
            "vt5s3tf8o6",
            "b0rgm58qsn",
            "9m59if353m",
            "biw2l9ok17",
            "b9ct0ue14d",
            "oxj0vxjsti",
            "f3k8w9vb49",
            "ec7v82k6nk",
            "f49ufwaqx7",
        ]

        start_weight = 30
        step_weight = 250
        finish_weight = start_weight + len(values) * step_weight
        weights = list(range(start_weight, finish_weight, step_weight))
        fill_up_weight = cardinality - sum(weights)
        if fill_up_weight > 0:
            values.append(HIDDEN_STRING_VALUE)
            weights.append(fill_up_weight)

        distr = RandomDistribution.choice(values, weights)

        return config.CollectionTemplate(
            name=name,
            fields=[
                config.FieldTemplate(
                    name="indexed_choice",
                    data_type=config.DataType.STRING,
                    distribution=distr,
                    indexed=True,
                ),
                config.FieldTemplate(
                    name="int1",
                    data_type=config.DataType.INTEGER,
                    distribution=distributions["int_normal"],
                    indexed=True,
                ),
                config.FieldTemplate(
                    name="non_indexed_choice",
                    data_type=config.DataType.STRING,
                    distribution=distributions["string_choice"],
                    indexed=False,
                ),
                config.FieldTemplate(
                    name="uniform1",
                    data_type=config.DataType.STRING,
                    distribution=distributions["string_uniform"],
                    indexed=False,
                ),
                config.FieldTemplate(
                    name="int2",
                    data_type=config.DataType.INTEGER,
                    distribution=distributions["int_normal"],
                    indexed=True,
                ),
                config.FieldTemplate(
                    name="choice2",
                    data_type=config.DataType.STRING,
                    distribution=distributions["string_choice"],
                    indexed=False,
                ),
                config.FieldTemplate(
                    name="mixed2",
                    data_type=config.DataType.STRING,
                    distribution=distributions["string_mixed"],
                    indexed=False,
                ),
            ],
            compound_indexes=[],
            cardinalities=[cardinality],
        )

    col_end2end = create_end2end_collection_template("end2end", 2000000)

    data_generator_config = config.DataGeneratorConfig(
        enabled=True,
        create_indexes=True,
        batch_size=10000,
        collection_templates=[col_end2end],
        write_mode=config.WriteMode.REPLACE,
        collection_name_with_card=True,
    )

    workload_execution_config = config.WorkloadExecutionConfig(
        enabled=True,
        output_collection_name="end2endData",
        write_mode=config.WriteMode.APPEND,
        warmup_runs=3,
        runs=30,
    )

    # The cost model to test.
    cost_model = CostModelCoefficients(
        scan_incremental_cost=422.31145989,
        scan_startup_cost=6175.527218993269,
        index_scan_incremental_cost=403.68075869,
        index_scan_startup_cost=14054.983953111061,
        seek_cost=1223.35513997,
        seek_startup_cost=7488.662376624863,
        filter_incremental_cost=83.7274685,
        filter_startup_cost=1461.3148783443378,
        eval_incremental_cost=430.6176946,
        eval_startup_cost=1103.4048573163343,
        group_by_incremental_cost=413.07932374,
        group_by_startup_cost=1199.8878012735659,
        unwind_incremental_cost=586.57200195,
        unwind_startup_cost=1.0,
        nested_loop_join_incremental_cost=161.62301944,
        nested_loop_join_startup_cost=402.8455479458652,
        hash_join_incremental_cost=250.61365634,
        hash_join_startup_cost=1.0,
        merge_join_incremental_cost=111.23423304,
        merge_join_startup_cost=1517.7970800404169,
        unique_incremental_cost=269.71368614,
        unique_startup_cost=1.0,
        union_incremental_cost=111.94945268,
        union_startup_cost=69.88096657391543,
        limit_skip_incremental_cost=62.42111111,
        limit_skip_startup_cost=655.1342592592522,
    )

    cost_estimator = CostEstimator(cost_model)

    processor_config = config.End2EndProcessorConfig(
        enabled=True,
        estimator=cost_estimator.estimate,
        input_collection_name=workload_execution_config.output_collection_name,
    )

    return config.EntToEndTestingConfig(
        database=main_config.database,
        data_generator=data_generator_config,
        workload_execution=workload_execution_config,
        processor=processor_config,
        result_csv_filepath="end2end.csv",
    )


async def execute_queries(
    database: DatabaseInstance,
    we_config: config.WorkloadExecutionConfig,
    collections: Sequence[CollectionInfo],
):
    collection = [ci for ci in collections if ci.name.startswith("end2end")][0]

    requests = []

    limits = [5, 10, 15, 20, 25, 50]
    skips = [15, 10, 5]

    for field in [f for f in collection.fields if f.name == "indexed_choice"]:
        for val in field.distribution.get_values():
            if val.startswith("_"):
                continue
            limit = limits[len(requests) % len(limits)]
            skip = skips[len(requests) % len(skips)]
            requests.append(
                Query(
                    pipeline=[
                        {"$match": {field.name: val}},
                        {"$skip": skip},
                        {"$limit": limit},
                        {"$project": {"int1": 1}},
                    ]
                )
            )

    for field in [f for f in collection.fields if f.name == "non_indexed_choice"]:
        for val in ["chisquare", "hi"]:
            limit = limits[len(requests) % len(limits)]
            skip = skips[len(requests) % len(skips)]
            requests.append(
                Query(
                    pipeline=[
                        {"$match": {field.name: val}},
                        {"$skip": skip},
                        {"$limit": limit},
                        {"$project": {"int1": 1}},
                    ]
                )
            )

    for i in range(100, 1000, 250):
        limit = limits[len(requests) % len(limits)]
        skip = skips[len(requests) % len(skips)]

        requests.append(
            Query(
                pipeline=[
                    {"$match": {"in1": i, "in2": 1000 - i}},
                    {"$skip": skip},
                    {"$limit": limit},
                ]
            )
        )

        requests.append(
            Query(
                pipeline=[
                    {"$match": {"in1": {"$lte": i}, "in2": 1000 - i}},
                    {"$skip": skip},
                    {"$limit": limit},
                ]
            )
        )

    await workload_execution.execute(database, we_config, [collection], requests)


async def execute_index_intersect_queries(
    database: DatabaseInstance,
    we_config: config.WorkloadExecutionConfig,
    collections: Sequence[CollectionInfo],
):
    collection = [ci for ci in collections if ci.name.startswith("end2end")][0]

    requests = []

    limits = [5, 10, 15, 20, 25, 50]
    skips = [15, 10, 5]

    for i in range(100, 1000, 250):
        limit = limits[len(requests) % len(limits)]
        skip = skips[len(requests) % len(skips)]

        requests.append(
            Query(
                pipeline=[
                    {"$match": {"in1": i, "in2": 1000 - i}},
                    {"$skip": skip},
                    {"$limit": limit},
                ]
            )
        )

        requests.append(
            Query(
                pipeline=[
                    {"$match": {"in1": {"$lte": i}, "in2": 1000 - i}},
                    {"$skip": skip},
                    {"$limit": limit},
                ]
            )
        )

    async with (
        get_database_parameter(database, "internalCostModelCoefficients") as cost_model_param,
    ):
        await cost_model_param.set('{"filterIncrementalCost": 10000.0}')
        await workload_execution.execute(database, we_config, [collection], requests)


def extract_abt_nodes(df: pd.DataFrame, estimate_cost) -> pd.DataFrame:
    """Extract ABT Nodes and execution statistics from calibration DataFrame."""

    def extract(df_seq):
        es_dict = exp.extract_execution_stats(df_seq["sbe"], df_seq["abt"], [])

        rows = []
        for abt_type, es in es_dict.items():
            for stat in es:
                if stat.n_processed == 0:
                    continue
                estimated_cost = estimate_cost(abt_type, stat.n_processed)
                rows.append(
                    EndToEndStatisticsRow(
                        abt_type=abt_type,
                        execution_time=stat.execution_time,
                        estimated_cost=estimated_cost,
                        n_documents=stat.n_processed,
                    )
                )
        return rows

    return pd.DataFrame(list(df.apply(extract, axis=1).explode()))


def build_abt_nodes_report(df: pd.DataFrame, processor_config: config.End2EndProcessorConfig):
    return extract_abt_nodes(df, processor_config.estimator)


def build_queries_report(df: pd.DataFrame, processor_config: config.End2EndProcessorConfig):
    abt_estimator = AbtCostEstimator(processor_config.estimator)

    def calculate_cost(row):
        rows = []
        estimations = []
        total_estimated_cost = abt_estimator.estimate(row["abt"], row["sbe"], estimations)
        local_id = 0
        rows.append(
            EndToEndStatisticsRow(
                pipeline=row["pipeline"],
                abt_type_id=local_id,
                execution_time=row["total_execution_time"],
                estimated_cost=total_estimated_cost,
            )
        )
        for abt_type, stats, local_cost in estimations:
            local_id += 1
            rows.append(
                EndToEndStatisticsRow(
                    pipeline=row["pipeline"],
                    abt_type=abt_type,
                    abt_type_id=local_id,
                    execution_time=row["total_execution_time"],
                    estimated_cost=local_cost,
                    n_documents=stats.n_processed,
                )
            )
        return rows

    return pd.DataFrame(list(df.apply(calculate_cost, axis=1).explode()))


async def conduct_end2end(
    database: DatabaseInstance, processor_config: config.End2EndProcessorConfig
):
    if not processor_config.enabled:
        return {}

    df = await exp.load_calibration_data(database, processor_config.input_collection_name)
    noout_df = exp.remove_outliers(df, 0.0, 0.90)

    abt_report = build_abt_nodes_report(noout_df, processor_config)

    queries_report = build_queries_report(noout_df, processor_config)

    report = pd.concat([abt_report, queries_report], axis=0)

    group_columns = ["pipeline", "abt_type", "abt_type_id"]

    def calc_r2(group):
        return r2_score(group["execution_time"], group["estimated_cost"])

    r2_scores = report.groupby(group_columns).apply(calc_r2).reset_index()
    r2_scores.columns = [group_columns + ["r2"], [""] * (len(group_columns) + 1)]

    agg_stats = report.groupby(group_columns)[
        [
            "execution_time",
            "estimated_cost",
            "estimation_error",
            "estimation_error_per_doc",
            "relative_error",
        ]
    ].agg([np.mean, np.std, np.min, np.max])

    report = pd.merge(r2_scores, agg_stats, on=group_columns)
    del report["abt_type_id"]
    return report


async def end2end(e2e_config: config.EntToEndTestingConfig):
    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    # 1. Database Instance provides connectivity to a MongoDB instance, it loads data optionally
    # from the dump on creating and stores data optionally to the dump on closing.
    with DatabaseInstance(e2e_config.database) as database:
        # 2. Data generation (optional), generates random data and populates collections with it.
        generator = DataGenerator(database, e2e_config.data_generator)
        await generator.populate_collections()

        # 3. Collecting data for calibration (optional).
        # It runs the pipelines and stores explains to the database.
        execution_query_functions = [execute_queries, execute_index_intersect_queries]
        for execute_query in execution_query_functions:
            await execute_query(database, e2e_config.workload_execution, generator.collection_infos)
            e2e_config.workload_execution.write_mode = config.WriteMode.APPEND

        # 4. Process end to end testing. Compare the estimated and actual costs and return results.
        report = await conduct_end2end(database, e2e_config.processor)
        if e2e_config.result_csv_filepath is not None:
            report.to_csv(e2e_config.result_csv_filepath, index=False)


async def main():
    e2e_config = make_config()
    await end2end(e2e_config)


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
