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

import parameters_extractor_classic
import qsn_calibrator
import workload_execution
from calibration_settings import main_config
from config import WriteMode
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


async def execute_index_scan_queries(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collection = [ci for ci in collections if ci.name.startswith("index_scan")][0]
    fields = [f for f in collection.fields if f.name == "choice"]

    requests = []

    for field in fields:
        for val in field.distribution.get_values():
            if val.startswith("_"):
                continue
            keys_length = len(val) + 2
            requests.append(
                Query(
                    pipeline=[{"$match": {field.name: val}}],
                    keys_length_in_bytes=keys_length,
                    note="IndexScan",
                )
            )

    await workload_execution.execute(
        database, main_config.workload_execution, [collection], requests
    )


async def execute_physical_scan_queries(
    database: DatabaseInstance, collections: Sequence[CollectionInfo]
):
    collections = [ci for ci in collections if ci.name.startswith("physical_scan")]
    fields = [f for f in collections[0].fields if f.name == "choice"]
    requests = []
    for field in fields:
        for val in field.distribution.get_values()[::3]:
            if val.startswith("_"):
                continue
            keys_length = len(val) + 2
            requests.append(
                Query(
                    pipeline=[{"$match": {field.name: val}}, {"$limit": 10}],
                    keys_length_in_bytes=keys_length,
                    note="PhysicalScan",
                )
            )

    await workload_execution.execute(
        database, main_config.workload_execution, collections, requests
    )


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


async def execute_limits(database: DatabaseInstance, collections: Sequence[CollectionInfo]):
    collection = [c for c in collections if c.name.startswith("index_scan")][0]
    limits = [1, 2, 5, 10, 15, 20, 25, 50]  # , 100, 250, 500, 1000, 2500, 5000, 10000]

    requests = [Query([{"$limit": limit}], note="Limit") for limit in limits]
    await workload_execution.execute(
        database, main_config.workload_execution, [collection], requests
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
        execution_query_functions = [execute_limits]
        for execute_query in execution_query_functions:
            await execute_query(database, generator.collection_infos)
            main_config.workload_execution.write_mode = WriteMode.APPEND
        # Calibration phase (optional).
        # Reads the explains stored on the previous step (this run and/or previous runs),
        # parses the explains, and calibrates the cost model for the QS nodes.
        models = await qsn_calibrator.calibrate(main_config.qs_calibrator, database)
        for qsn, model in models.items():
            print(f"{qsn}\t\t{model}")

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
