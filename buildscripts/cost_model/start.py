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
"""Cost Model Calibrator entry point."""

import asyncio
import csv
import dataclasses
import os
from typing import Mapping, Sequence

import numpy as np
import parameters_extractor_classic
import qsn_calibrator
import workload_execution
from calibration_settings import main_config
from config import DataType, WriteMode
from cost_estimator import CostModelParameters, ExecutionStats
from data_generator import CollectionInfo, DataGenerator
from database_instance import DatabaseInstance, get_database_parameter
from workload_execution import Query, QueryParameters

__all__ = []


def save_to_csv(parameters: Mapping[str, Sequence[CostModelParameters]], filepath: str) -> None:
    """Save model input parameters to a csv file."""
    qsn_type_name = "qsn_type"
    fieldnames = [
        qsn_type_name,
        *[f.name for f in dataclasses.fields(ExecutionStats)],
        *[f.name for f in dataclasses.fields(QueryParameters)],
    ]
    with open(filepath, "w", newline="") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        for qsn_type, type_params_list in parameters.items():
            for type_params in type_params_list:
                fields = dataclasses.asdict(type_params.execution_stats) | dataclasses.asdict(
                    type_params.query_params
                )
                fields[qsn_type_name] = qsn_type
                writer.writerow(fields)


async def execute_index_seeks(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [c for c in collections if c.name == "index_scan_10000"]
    assert len(collections) == 1

    requests = []
    cards = [25, 50, 100, 200, 300, 400, 500] + list(range(1000, 10_001, 1000))
    # For every query, we run it as both a forward and backward scan.
    for direction, note in [(1, "FORWARD"), (-1, "BACKWARD")]:
        for card in cards:
            requests.append(
                Query(
                    {"filter": {"a": {"$lt": card}}, "sort": {"a": direction}, "hint": {"a": 1}},
                    note=f"IXSCAN_{note}",
                    expected_stage={"IXSCAN": {"direction": note.lower()}},
                )
            )

            # In order to calibrate the cost of seeks, we uniformly sample for an $in query so that the
            # index scan will examine the same number of keys as the range query,
            # but instead of being able to traverse the leaves, it has to do a seek for each one.
            # The reason for the `// 2` is because on each seek it examines 2 keys, after the first one it additionally checks the next key
            # to try and avoid an unnecessary seek. Lastly, the casting is due to BSON not understanding numpy integer types.
            seeks = [
                int(key)
                for key in np.linspace(
                    0,
                    collections[0].documents_count,
                    endpoint=False,
                    dtype=np.dtype(int),
                    # We need this max as otherwise we will generate an empty $in query (which turns into an EOF plan) for
                    # cardinality 1.
                    num=max(1, card // 2),
                )
            ]
            requests.append(
                Query(
                    {"filter": {"a": {"$in": seeks}}, "sort": {"a": direction}, "hint": {"a": 1}},
                    note=f"IXSCAN_{note}",
                    expected_stage={"IXSCAN": {"direction": note.lower()}},
                )
            )

            if direction == 1:
                # In order to calibrate the cost of a filter on an ixscan, we add a predicate to the
                # queries above that will always be true. We expect that the cost of a filter on an
                # ixscan should be the same whether the direction is forwards or backwards, so we only
                # calibrate in the forwards case.
                requests.append(
                    Query(
                        {
                            "filter": {
                                "a": {"$lt": card, "$mod": [1, 0]},
                            },
                            "sort": {"a": direction},
                            "hint": {"a": 1},
                        },
                        note="IXSCAN_W_FILTER",
                        expected_stage={
                            "IXSCAN": {
                                "direction": note.lower(),
                                "filter": {"a": {"$mod": [1, 0]}},
                            }
                        },
                    )
                )
                requests.append(
                    Query(
                        {
                            "filter": {"a": {"$in": seeks, "$mod": [1, 0]}},
                            "sort": {"a": direction},
                            "hint": {"a": 1},
                        },
                        note="IXSCAN_W_FILTER",
                        expected_stage={
                            "IXSCAN": {"direction": note.lower(), "filter": {"a": {"$mod": [1, 0]}}}
                        },
                    )
                )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_collection_scans(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [c for c in collections if c.name == "doc_scan_200000"]
    assert len(collections) == 1

    # We use higher numbers here to be representative of how COLLSCANs are used and to avoid
    # the instability we experienced when using smaller numbers.
    limits = [100_000, 110_000, 120_000, 130_000, 140_000, 150_000]
    requests = []
    for direction, dir_text in [(1, "FORWARD"), (-1, "BACKWARD")]:
        note = f"COLLSCAN_{dir_text}"
        for limit in limits:
            requests.append(
                Query(
                    {"limit": limit, "sort": {"$natural": direction}},
                    note=note,
                    expected_stage={"COLLSCAN": {"direction": dir_text.lower()}},
                )
            )

            if direction == 1:
                # We expect that the cost of a filter on a collscan should be the same whether
                # the direction is forwards or backwards, so we only calibrate in the forwards case.
                requests.append(
                    Query(
                        {
                            "limit": limit,
                            "filter": {"int_uniform_unindexed_0": {"$gt": 0}},
                            "sort": {"$natural": direction},
                        },
                        note="COLLSCAN_W_FILTER",
                        expected_stage={
                            "COLLSCAN": {
                                "direction": dir_text.lower(),
                                "filter": {"int_uniform_unindexed_0": {"$gt": 0}},
                            }
                        },
                    )
                )
    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_limits(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [c for c in collections if c.name == "index_scan_10000"]
    assert len(collections) == 1

    limits = [1, 2, 5, 10, 15, 20, 25, 50, 100, 250, 500] + list(range(1000, 10001, 1000))

    requests = [
        Query(
            {"limit": limit},
            note="LIMIT",
            expected_stage="LIMIT",
        )
        for limit in limits
    ]
    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_skips(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [c for c in collections if c.name == "index_scan_10000"]
    assert len(collections) == 1

    skips = [5, 10, 15, 20, 25, 50, 75, 100, 500, 1000, 2500, 5000]
    limits = [5, 10, 15, 20, 50, 75, 100, 250, 500, 1000]
    requests = []
    # We add a LIMIT on top of the SKIP in order to easily vary the number of processed documents.
    for limit in limits:
        for skip in skips:
            requests.append(
                Query(
                    find_cmd={"skip": skip, "limit": limit},
                    note="SKIP",
                    expected_stage="SKIP",
                )
            )
    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_projections(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [c for c in collections if c.name == "projection_30000"]
    assert len(collections) == 1

    limits = [5, 10, 50, 75, 100, 150, 300, 500] + list(range(1000, 10001, 1000))

    # We calibrate using projections on the last field since this means the node does a nontrivial amount of work.
    # This is because non-covered projections iterate over the fields in a given document as part of its work.
    field = collections[0].fields[-1]
    requests = []
    # Simple projections, these do not contain any computed fields and are not fully covered by an index.
    for limit in limits:
        requests.append(
            Query(
                {"limit": limit, "projection": {field.name: 1}},
                note="PROJECTION_SIMPLE",
                expected_stage="PROJECTION_SIMPLE",
            )
        )

    # Covered projections, these are inclusions that are fully covered by an index.
    field = [f for f in collections[0].fields if f.indexed][-1]
    for limit in limits:
        requests.append(
            Query(
                {"limit": limit, "projection": {"_id": 0, field.name: 1}, "hint": {field.name: 1}},
                note="PROJECTION_COVERED",
                expected_stage="PROJECTION_COVERED",
            )
        )

    # Default projections, these are the only ones that can handle computed projections,
    # so that is how we calibrate them. We assume that the computation will be constant across
    # the enumerated plans and thus keep it very simple.
    fields = [f for f in collections[0].fields if f.type == DataType.INTEGER]
    for limit in limits:
        requests.append(
            Query(
                {"limit": limit, "projection": {"out": {"$add": [f"${f.name}" for f in fields]}}},
                note="PROJECTION_DEFAULT",
                expected_stage="PROJECTION_DEFAULT",
            )
        )
    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_sorts(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    # Using collections of varying sizes instead of limits, as the limit + sort combination
    # would trigger the optimized top-K sorting algorithm, which is calibrated separately below.
    collections = [c for c in collections if c.name.startswith("sort")]
    assert len(collections) == 11

    requests = [
        # A standard sort applies the simple sort algorithm.
        Query({"sort": {"payload": 1}}, note="SORT_SIMPLE", expected_stage="SORT"),
        # Including the recordId explicitly forces the use of the default sort algorithm.
        Query(
            {"projection": {"$recordId": {"$meta": "recordId"}}, "sort": {"payload": 1}},
            note="SORT_DEFAULT",
            expected_stage="SORT",
        ),
    ]

    # By combining a sort with a limit, we trigger the top-K sorting algorithm, which works
    # for both the simple and default sort algorithms.
    limits = [2, 5, 10, 50, 75, 100, 150, 300, 500, 1000]
    for limit in limits:
        requests.append(
            Query(
                {"sort": {"payload": 1}, "limit": limit},
                note="SORT_LIMIT_SIMPLE",
                expected_stage="SORT",
            )
        )
        requests.append(
            Query(
                {
                    "projection": {"$recordId": {"$meta": "recordId"}},
                    "sort": {"payload": 1},
                    "limit": limit,
                },
                note="SORT_LIMIT_DEFAULT",
                expected_stage="SORT",
            )
        )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_sorts_spill(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [c for c in collections if c.name.startswith("large_sort")]
    assert len(collections) == 6

    requests = [
        # A standard sort applies the simple sort algorithm.
        Query(
            {"sort": {"payload": 1}},
            note="SORT_SIMPLE_SPILL",
            expected_stage={"SORT": {"usedDisk": True}},
        ),
        # Including the recordId explicitly forces the use of the default sort algorithm.
        Query(
            {"projection": {"$recordId": {"$meta": "recordId"}}, "sort": {"payload": 1}},
            note="SORT_DEFAULT_SPILL",
            expected_stage={"SORT": {"usedDisk": True}},
        ),
    ]

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_merge_sorts(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [c for c in collections if c.name.startswith("merge_sort")]
    assert len(collections) == 11

    fields = collections[0].fields

    requests = []
    for num_merge_inputs in range(2, len(fields)):
        requests.append(
            Query(
                find_cmd={
                    "filter": {"$or": [{f.name: 1} for f in fields[:num_merge_inputs]]},
                    "sort": {"sort_field": 1},
                },
                note="SORT_MERGE",
                expected_stage="SORT_MERGE",
            )
        )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_ors(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    # Using collections of varying sizes instead of limits, as a limit would prevent subsequent
    # OR branches from being executed if earlier branches already satisfy the limit requirement.
    collections = [c for c in collections if c.name.startswith("or")]
    assert len(collections) == 20

    requests = [
        Query(
            find_cmd={"filter": {"$or": [{"a": 1}, {"b": 1}]}},
            note="OR",
            expected_stage="OR",
        )
    ]

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_sort_intersections(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [ci for ci in collections if ci.name.startswith("intersection_sorted")]
    assert len(collections) == 4

    # Values ranging from 1 to 10
    values = collections[0].fields[0].distribution.get_values()

    requests = []
    for i in values:
        for j in values:
            requests.append(
                Query(
                    find_cmd={"filter": {"a": i, "b": j}},
                    note="AND_SORTED",
                    expected_stage="AND_SORTED",
                )
            )

    async with (
        get_database_parameter(
            database, "internalQueryForceIntersectionPlans"
        ) as force_intersection_param,
        get_database_parameter(
            database, "internalQueryPlannerEnableSortIndexIntersection"
        ) as enable_sort_intersection_param,
    ):
        await force_intersection_param.set(True)
        await enable_sort_intersection_param.set(True)
        await workload_execution.execute(
            database, main_config.workload_execution, collections, requests
        )


async def execute_hash_intersections(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [ci for ci in collections if ci.name == "intersection_hash_1000"]
    assert len(collections) == 1

    # Values ranging from 1 to 10
    values = collections[0].fields[0].distribution.get_values()

    requests = []
    for i in values:
        for j in values:
            requests.append(
                Query(
                    find_cmd={"filter": {"a": {"$lte": i}, "b": {"$lte": j}}},
                    note="AND_HASH",
                    expected_stage="AND_HASH",
                )
            )

    async with (
        get_database_parameter(
            database, "internalQueryForceIntersectionPlans"
        ) as force_intersection_param,
        get_database_parameter(
            database, "internalQueryPlannerEnableHashIntersection"
        ) as enable_hash_intersection_param,
    ):
        await force_intersection_param.set(True)
        await enable_hash_intersection_param.set(True)
        await workload_execution.execute(
            database, main_config.workload_execution, collections, requests
        )


async def execute_fetches(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collections = [c for c in collections if c.name == "doc_scan_100000"]
    assert len(collections) == 1

    requests = []

    cards = [10, 50, 100, 500, 1000, 5000, 10000, 15000]
    for card in cards:
        requests.append(
            Query(
                {"filter": {"int_uniform": {"$lt": card}}},
                note="FETCH",
                expected_stage="FETCH",
            )
        )

        requests.append(
            Query(
                # 'int_uniform_unindexed_0' is not indexed, so the fetch will have a filter.
                {
                    "filter": {
                        "int_uniform": {"$lt": card},
                        "int_uniform_unindexed_0": {"$gt": 0},
                    }
                },
                note="FETCH_W_FILTER",
                expected_stage={"FETCH": {"filter": {"int_uniform_unindexed_0": {"$gt": 0}}}},
            )
        )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_index_scans_w_diff_num_fields(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [c for c in collections if c.name == "index_scan_10000"]
    assert len(collections) == 1

    requests = []

    # The compound_indexes list does not contain the single-field index {a: 1}.
    for index in ["a"] + collections[0].compound_indexes:
        hint_obj = {key: 1 for key in index}

        requests.append(
            Query(
                {"filter": {"a": {"$lt": 10000}}, "hint": hint_obj},
                note="IXSCANS_W_DIFF_NUM_FIELDS",
                expected_stage="IXSCAN",
            )
        )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_fetch_w_filters_w_diff_num_leaves(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [c for c in collections if c.name == "doc_scan_100000"]
    assert len(collections) == 1

    requests = []

    unindexed_fields = [field.name for field in collections[0].fields if "unindexed" in field.name]
    assert len(unindexed_fields) == 10

    for fields_w_preds in [unindexed_fields[:i] for i in range(1, len(unindexed_fields) + 1)]:
        # We build up queries of the shape
        # {'int_uniform_unindexed_0': {'$gt': 0}, 'int_uniform': {'$lt': 50000}}},
        # {'int_uniform_unindexed_0': {'$gt': 0}, 'int_uniform_unindexed_1': {'$gt': 0}, 'int_uniform': {'$lt': 50000}}}
        # and so on, until we have all 10 unindexed fields in the filter.
        filter = {f: {"$gt": 0} for f in fields_w_preds}
        filter["int_uniform"] = {"$lt": 50000}

        requests.append(
            Query(
                {"filter": filter},
                note="FETCH_W_FILTERS_W_DIFF_NUM_LEAVES",
                expected_stage={
                    "FETCH": {
                        "filter": {fields_w_preds[0]: {"$gt": 0}}
                        if len(fields_w_preds) == 1
                        else {"$and": [{k: v} for k, v in filter.items() if k != "int_uniform"]}
                    }
                },
            )
        )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_collscan_w_filters_w_diff_num_leaves(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [c for c in collections if c.name == "doc_scan_100000"]
    assert len(collections) == 1

    requests = []

    unindexed_fields = [field.name for field in collections[0].fields if "unindexed" in field.name]
    assert len(unindexed_fields) == 10

    for fields_w_preds in [unindexed_fields[:i] for i in range(1, len(unindexed_fields) + 1)]:
        # We build up queries of the shape
        # {'int_uniform_unindexed_0': {'$gt': 0}},
        # {'int_uniform_unindexed_0': {'$gt': 0}, 'int_uniform_unindexed_1': {'$gt': 0}}
        # and so on, until we have all 10 unindexed fields in the filter.
        filter = {f: {"$gt": 0} for f in fields_w_preds}

        requests.append(
            Query(
                {"filter": filter, "sort": {"$natural": 1}, "limit": 50000},
                note="COLLSCAN_W_FILTERS_W_DIFF_NUM_LEAVES",
                expected_stage={
                    "COLLSCAN": {
                        "filter": {fields_w_preds[0]: {"$gt": 0}}
                        if len(fields_w_preds) == 1
                        else {"$and": [{k: v} for k, v in filter.items()]}
                    }
                },
            )
        )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_ixscan_w_filters_w_diff_num_leaves(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [c for c in collections if c.name == "index_scan_10000"]
    assert len(collections) == 1

    requests = []

    field_names = [chr(ord("a") + i) for i in range(10)]

    # Note we do not include a filter that has only one leaf. We noticed that there is a
    # large jump between 1 and 2 leaves for the cost of an ixscan filter, so we omitted
    # it to get a better fit.
    for fields_w_preds in [field_names[:i] for i in range(2, len(field_names) + 1)]:
        # We build up queries of the shape
        # {'a': {"$mod": [1, 0]}, 'b': {"$mod": [1, 0]}},
        # {'a': {"$mod": [1, 0]}, 'b': {"$mod": [1, 0]}, 'c': {"$mod": [1, 0]}},
        # and so on, until we have all 10 fields in the filter.
        filter = {f: {"$mod": [1, 0]} for f in fields_w_preds}

        requests.append(
            Query(
                # hint the compound index on {a: 1, b: 1, ... j: 1}
                {"filter": filter, "hint": {k: 1 for k in field_names}},
                note="IXSCAN_W_FILTERS_W_DIFF_NUM_LEAVES",
                expected_stage={
                    "IXSCAN": {
                        "filter": {fields_w_preds[0]: {"$mod": [1, 0]}}
                        if len(fields_w_preds) == 1
                        else {"$and": [{k: v} for k, v in filter.items()]}
                    }
                },
            )
        )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def main():
    """Entry point function."""
    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    # 1. Database Instance provides connectivity to a MongoDB instance, it loads data optionally
    # from the dump on creating and stores data optionally to the dump on closing.
    with DatabaseInstance(main_config.database) as database:
        # 2. Data generation (optional), generates random data and populates collections with it.
        generator = DataGenerator(database, main_config.data_generator)
        await generator.populate_collections()
        # 3. Collecting data for calibration (optional).
        # It runs the pipelines and stores explains to the database.
        execution_query_functions = [
            execute_index_seeks,
            execute_projections,
            execute_collection_scans,
            execute_limits,
            execute_skips,
            execute_sorts,
            execute_sorts_spill,
            execute_merge_sorts,
            execute_ors,
            execute_sort_intersections,
            execute_hash_intersections,
            execute_fetches,
            execute_index_scans_w_diff_num_fields,
            execute_fetch_w_filters_w_diff_num_leaves,
            execute_collscan_w_filters_w_diff_num_leaves,
            execute_ixscan_w_filters_w_diff_num_leaves,
        ]
        for execute_query in execution_query_functions:
            await execute_query(database, generator.collection_infos)
            main_config.workload_execution.write_mode = WriteMode.APPEND
        # Calibration phase (optional).
        # Reads the explains stored on the previous step (this run and/or previous runs),
        # parses the explains, and calibrates the cost model for the QS nodes.
        models = await qsn_calibrator.calibrate(main_config.qs_calibrator, database)
        # Pad all QSN names to be nice and pretty.
        pad = max(len(node) for node in models) + 8
        for qsn, model in models.items():
            print(f"{qsn:<{pad}}{model}")

        parameters = await parameters_extractor_classic.extract_parameters(
            main_config.qs_calibrator, database, []
        )
        save_to_csv(parameters, "parameters.csv")

    print("DONE!")


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
