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
from database_instance import DatabaseInstance
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


async def execute_index_intersections_with_requests(
    database: DatabaseInstance, collections: Sequence[CollectionInfo], requests: Sequence[Query]
):
    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )

    main_config.workload_execution.write_mode = WriteMode.APPEND
    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests[::4]
    )


async def execute_index_intersections(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [ci for ci in collections if ci.name.startswith("c_int")]

    requests = []

    for i in range(0, 1000, 100):
        requests.append(Query(pipeline=[{"$match": {"in1": i, "in2": i}}], keys_length_in_bytes=1))

        requests.append(
            Query(pipeline=[{"$match": {"in1": i, "in2": 1000 - i}}], keys_length_in_bytes=1)
        )

        requests.append(
            Query(
                pipeline=[{"$match": {"in1": {"$lte": i}, "in2": 1000 - i}}], keys_length_in_bytes=1
            )
        )

        requests.append(
            Query(
                pipeline=[{"$match": {"in1": i, "in2": {"$gt": 1000 - i}}}], keys_length_in_bytes=1
            )
        )

    await execute_index_intersections_with_requests(database, collections, requests)


async def execute_index_seeks(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collection = [c for c in collections if c.name.startswith("index_scan")][0]
    field = [f for f in collection.fields if f.name == "int_uniform"][0]
    requests = []
    cards = [25, 50, 100, 200, 300]
    # For every query, we run it as both a forward and backward scan.
    for direction, note in [(1, "FORWARD"), (-1, "BACKWARD")]:
        for card in cards:
            requests.append(
                Query(
                    {"filter": {field.name: {"$lt": card}}, "sort": {field.name: direction}},
                    note=f"IXSCAN_{note}",
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
                    collection.documents_count,
                    endpoint=False,
                    dtype=np.dtype(int),
                    # We need this max as otherwise we will generate an empty $in query (which turns into an EOF plan) for
                    # cardinality 1.
                    num=max(1, card // 2),
                )
            ]
            requests.append(
                Query(
                    {"filter": {field.name: {"$in": seeks}}, "sort": {field.name: direction}},
                    note=f"IXSCAN_{note}",
                )
            )
    await workload_execution.execute(
        database, main_config.workload_execution, [collection], requests
    )


async def execute_collection_scans(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [c for c in collections if c.name.startswith("coll_scan")]
    # Even though these numbers are not representative of the way COLLSCANs are usually used,
    # we can use them for calibration based on the assumption that the cost scales linearly.
    limits = [5, 10, 50, 75, 100, 150, 300, 500, 1000]
    requests = []
    for direction in [1, -1]:
        note = f"COLLSCAN_{'FORWARD' if direction == 1 else 'BACKWARD'}"
        for limit in limits:
            requests.append(Query({"limit": limit, "sort": {"$natural": direction}}, note=note))
    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


async def execute_limits(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collection = [c for c in collections if c.name.startswith("index_scan")][0]
    limits = [1, 2, 5, 10, 15, 20, 25, 50, 100, 250, 500, 1000]

    requests = [Query({"limit": limit}, note="LIMIT") for limit in limits]
    await workload_execution.execute(
        database, main_config.workload_execution, [collection], requests
    )


async def execute_skips(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collection = [c for c in collections if c.name.startswith("index_scan")][0]
    skips = [5, 10, 15, 20, 25, 50, 75, 100, 500, 1000]
    limits = [5, 10, 15, 20, 50, 75, 100]
    requests = []
    # We add a LIMIT on top of the SKIP in order to easily vary the number of processed documents.
    for limit in limits:
        for skip in skips:
            requests.append(Query(find_cmd={"skip": skip, "limit": limit}, note="SKIP"))
    await workload_execution.execute(
        database, main_config.workload_execution, [collection], requests
    )


async def execute_projections(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collection = [c for c in collections if c.name.startswith("c_int_05_30")][0]
    limits = [5, 10, 50, 75, 100, 150, 300, 500, 1000]
    # We calibrate using projections on the last field since this means the node does a nontrivial amount of work.
    # This is because non-covered projections iterate over the fields in a given document as part of its work.
    field = collection.fields[-1]
    requests = []
    # Simple projections, these do not contain any computed fields and are not fully covered by an index.
    for limit in limits:
        requests.append(
            Query({"limit": limit, "projection": {field.name: 1}}, note="PROJECTION_SIMPLE")
        )

    # Covered projections, these are inclusions that are fully covered by an index.
    field = [f for f in collection.fields if f.indexed][-1]
    for limit in limits:
        requests.append(
            Query(
                {"limit": limit, "projection": {"_id": 0, field.name: 1}, "hint": {field.name: 1}},
                note="PROJECTION_COVERED",
            )
        )

    # Default projections, these are the only ones that can handle computed projections,
    # so that is how we calibrate them. We assume that the computation will be constant across
    # the enumerated plans and thus keep it very simple.
    fields = [f for f in collection.fields if f.type == DataType.INTEGER]
    for limit in limits:
        requests.append(
            Query(
                {"limit": limit, "projection": {"out": {"$add": [f"${f.name}" for f in fields]}}},
                note="PROJECTION_DEFAULT",
            )
        )
    await workload_execution.execute(
        database, main_config.workload_execution, [collection], requests
    )


async def execute_sorts(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    # Using collections of varying sizes instead of limits, as the limit + sort combination
    # would trigger the optimized top-N sorting algorithm, which requires separate calibration.
    collections = [c for c in collections if c.name.startswith("sort")]

    requests = [
        # A standard sort applies the simple sort algorithm.
        Query({"sort": {"payload": 1}}, note="SORT_SIMPLE"),
        # Including the recordId explicitly forces the use of the default sort algorithm.
        Query(
            {"projection": {"$recordId": {"$meta": "recordId"}}, "sort": {"payload": 1}},
            note="SORT_DEFAULT",
        ),
    ]
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
