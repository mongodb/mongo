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

import dataclasses
import os
import csv
import asyncio
from typing import Mapping, Sequence
from cost_estimator import ExecutionStats, ModelParameters
from data_generator import DataGenerator
from database_instance import DatabaseInstance
import abt_calibrator
import workload_execution
from workload_execution import Query, QueryParameters
import parameters_extractor
from calibration_settings import distributions, main_config

__all__ = []


def save_to_csv(parameters: Mapping[str, Sequence[ModelParameters]], filepath: str) -> None:
    """Save model input parameters to a csv file."""
    abt_type_name = 'abt_type'
    fieldnames = [
        abt_type_name, *[f.name for f in dataclasses.fields(ExecutionStats)],
        *[f.name for f in dataclasses.fields(QueryParameters)]
    ]
    with open(filepath, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        for abt_type, type_params_list in parameters.items():
            for type_params in type_params_list:
                fields = dataclasses.asdict(type_params.execution_stats) | dataclasses.asdict(
                    type_params.query_params)
                fields[abt_type_name] = abt_type
                writer.writerow(fields)


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
        requests = []
        for val in distributions['string_choice'].get_values():
            keys_length = len(val) + 2
            for i in range(1, 5):
                requests.append(
                    Query(pipeline=[{'$match': {f'choice{i}': val}}],
                          keys_length_in_bytes=keys_length))

        await workload_execution.execute(database, main_config.workload_execution,
                                         generator.collection_infos, requests)

        # Calibration phase (optional).
        # Reads the explains stored on the previous step (this run and/or previous runs),
        # aparses the explains, nd calibrates the cost model for the ABT nodes.
        models = await abt_calibrator.calibrate(
            main_config.abt_calibrator, database,
            ['IndexScan', 'Seek', 'PhysicalScan', 'ValueScan', 'CoScan', 'Scan'])
        for abt, model in models.items():
            print(abt)
            print(model)

        parameters = await parameters_extractor.extract_parameters(main_config.abt_calibrator,
                                                                   database, [])
        save_to_csv(parameters, 'parameters.csv')

    print("DONE!")


if __name__ == '__main__':
    loop = asyncio.get_event_loop()
    loop.run_until_complete(main())
