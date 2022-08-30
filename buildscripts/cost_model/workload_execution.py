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

from __future__ import annotations
from dataclasses import asdict, dataclass
import json
from typing import Sequence
from bson.objectid import ObjectId
from data_generator import CollectionInfo
from database_instance import DatabaseInstance, Pipeline
from config import WorkloadExecutionConfig, WriteMode

__all__ = ['execute']


@dataclass
class Query:
    """Query pipleline and related model input parameters."""

    pipeline: Pipeline
    keys_length_in_bytes: int


@dataclass
class QueryParameters:
    """Model input parameters specific for a workload query executed on some collection and used for calibration."""

    keys_length_in_bytes: int
    average_document_size_in_bytes: float

    def to_json(self) -> str:
        """Serialize the parameters to JSON."""
        return json.dumps(asdict(self))

    @staticmethod
    def from_json(json_str: str) -> QueryParameters:
        """Deserialize from JSON."""
        return QueryParameters(**json.loads(json_str))


async def execute(database: DatabaseInstance, config: WorkloadExecutionConfig,
                  collection_infos: Sequence[CollectionInfo], queries: Sequence[Query]):
    """Run the given queries and write the collected explain into collection."""
    if not config.enabled:
        return

    collector = WorkloadExecution(database, config)
    await collector.async_init()
    print('>>> running queries')
    await collector.collect(collection_infos, queries)


class WorkloadExecution:
    """Runs a number of queries to generate and collect execution statistics."""

    def __init__(self, database: DatabaseInstance, config: WorkloadExecutionConfig):
        self.database = database
        self.config = config

    async def async_init(self):
        """Initialize the database settings."""
        await self.database.enable_sbe(True)
        await self.database.enable_cascades(True)

        if self.config.write_mode == WriteMode.REPLACE:
            await self.database.drop_collection(self.config.output_collection_name)

    async def collect(self, collection_infos: Sequence[CollectionInfo], queries: Sequence[Query]):
        """Run the given piplelines on the given collection to generate and collect execution statistics."""
        measurements = []

        for coll_info in collection_infos:
            print(f'\n>>>>> running queries on collection {coll_info.name}')
            for query in queries:
                print(f'>>>>>>> running query {query.pipeline}')
                await self._run_query(coll_info, query, measurements)

        await self.database.insert_many(self.config.output_collection_name, measurements)

    async def _run_query(self, coll_info: CollectionInfo, query: Query, result: Sequence):
        # warm up
        for _ in range(self.config.warmup_runs):
            await self.database.explain(coll_info.name, query.pipeline)

        run_id = ObjectId()
        avg_doc_size = await self.database.get_average_document_size(coll_info.name)
        parameters = QueryParameters(keys_length_in_bytes=query.keys_length_in_bytes,
                                     average_document_size_in_bytes=avg_doc_size)
        for _ in range(self.config.runs):
            explain = await self.database.explain(coll_info.name, query.pipeline)
            if explain['ok'] == 1:
                result.append({
                    'run_id': run_id, 'collection': coll_info.name,
                    'pipeline': json.dumps(query.pipeline), 'explain': json.dumps(explain),
                    'query_parameters': parameters.to_json()
                })
