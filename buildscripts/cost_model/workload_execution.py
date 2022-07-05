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
"""Workload Execution. Run the queries to collect data for calibration."""

import json
from typing import Sequence
from database_instance import DatabaseInstance, Pipeline
from config import WorkloadExecutionConfig, WriteMode

__all__ = ['execute']


def execute(database: DatabaseInstance, config: WorkloadExecutionConfig,
            collection_names: Sequence[str], pipelines: Sequence[Pipeline]):
    """Run the given queries and write the collected explain into collection."""
    if not config.enabled:
        return

    collector = WorkloadExecution(database, config)
    # run with indexes enabled
    for collection_name in collection_names:
        database.unhide_all_indexes(collection_name)
    collector.collect(collection_names, pipelines)

    # run with indexes disabled
    for collection_name in collection_names:
        database.hide_all_indexes(collection_name)
    collector.collect(collection_names, pipelines)


class WorkloadExecution:
    """Runs a number of queries to generate and collect execution statistics."""

    def __init__(self, database: DatabaseInstance, config: WorkloadExecutionConfig):
        self.database = database
        self.config = config

        self.database.enable_sbe(True)
        self.database.enable_cascades(True)

        if self.config.write_mode == WriteMode.REPLACE:
            self.database.drop_collection(self.config.output_collection_name)

    def collect(self, collection_names: Sequence[str], pipelines: Sequence[Pipeline]):
        """Run the given piplelines on the given collection to generate and collect execution statistics."""
        measurements = []

        for collection_name in collection_names:
            for pipeline in pipelines:
                self._run_query(collection_name, pipeline, measurements)

        self.database.insert_many(self.config.output_collection_name, measurements)

    def _run_query(self, collection_name: str, pipeline: Pipeline, result: Sequence):
        # warm up
        for _ in range(self.config.warmup_runs):
            self.database.explain(collection_name, pipeline)

        for _ in range(self.config.runs):
            explain = self.database.explain(collection_name, pipeline)
            if explain['ok'] == 1:
                result.append({
                    'collection': collection_name, 'pipeline': json.dumps(pipeline),
                    'explain': json.dumps(explain)
                })
