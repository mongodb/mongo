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

import os
import json
from data_generator import DataGenerator
from database_instance import DatabaseInstance
from config import Config
import abt_calibrator
import workload_execution

__all__ = []


def main():
    """Entry point function."""
    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    with open("config.json") as config_file:
        config = Config.create(json.load(config_file))

    # 1. Database Instance provides connectivity to a MongoDB instance, it loads data optionally
    # from the dump on creating and stores data optionally to the dump on closing.
    with DatabaseInstance(config.database) as database:

        # 2. Data generation (optional), generates random data and populates collections with it.
        generator = DataGenerator(database, config.data_generator)
        generator.populate_collections()
        collection_names = list(generator.list_collection_names())

        # 3. Collecting data for calibration (optional).
        # It runs the pipelines and stores explains to the database.
        pipelines = [
            [{'$match': {'f_5': 7}}],
            [{'$match': {'f_1': 5}}],
            [{'$match': {'f_7': 4}}],
            [{'$match': {'f_5': 7}}],
            [{'$match': {'f_1': 5}}],
            [{'$match': {'f_2': generator.gen_random_string()}}],
            [{'$match': {'f_5': generator.gen_random_string()}}],
        ]
        workload_execution.execute(database, config.workload_execution, collection_names, pipelines)

        # Calibration phase (optional).
        # Reads the explains stored on the previous step (this run and/or previous runs),
        # aparses the explains, nd calibrates the cost model for the ABT nodes.
        models = abt_calibrator.calibrate(config.abt_calibrator, database, ['IndexScan', 'Seek'])
        for abt, model in models.items():
            print(abt)
            print(model)


if __name__ == '__main__':
    main()
