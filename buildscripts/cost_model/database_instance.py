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
"""A wrapper with useful methods over MongoDB database."""

from __future__ import annotations
import re
from typing import Sequence, Mapping, NewType, Any
import subprocess
from pymongo import MongoClient, InsertOne
from config import DatabaseConfig, RestoreMode

__all__ = ['DatabaseInstance', 'Pipeline']
"""MongoDB Aggregate's Pipeline"""
Pipeline = NewType('Pipeline', Sequence[Mapping[str, Any]])


class DatabaseInstance:
    """MongoDB Database wrapper."""

    def __init__(self, config: DatabaseConfig) -> None:
        """Initialize wrapper."""
        self.config = config
        self.client = MongoClient(config.connection_string)
        self.database = self.client.get_database(config.database_name)

    def __enter__(self):
        if self.config.restore_from_dump == RestoreMode.ALWAYS or (
                self.config.restore_from_dump == RestoreMode.ONLY_NEW
                and self.config.database_name not in self.client.database_names()):
            self.restore()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        if self.config.dump_on_exit:
            self.enable_cascades(False)
            self.dump()

    def drop(self):
        """Drop the database."""
        self.client.drop_database(self.config.database_name)

    def restore(self):
        """Restore the database from the 'self.dump_directory'."""
        subprocess.run(['mongorestore', '--nsInclude', f'{self.config.database_name}.*', '--drop'],
                       shell=True, check=True, cwd=self.config.dump_path)

    def dump(self):
        """Dump the database into 'self.dump_directory'."""
        subprocess.run(['mongodump', '--db', self.config.database_name], cwd=self.config.dump_path,
                       check=True)

    def enable_sbe(self, state: bool) -> None:
        """Enable new query execution engine. Throw pymongo.errors.OperationFailure in case of failure."""
        # self.client.admin.command({'setParameter': 1, 'internalQueryEnableSlotBasedExecutionEngine': state})
        self.client.admin.command({'setParameter': 1, 'internalQueryForceClassicEngine': not state})

    def enable_cascades(self, state: bool) -> None:
        """Enable new query optimizer. Requires featureFlagCommonQueryFramework set to True."""
        self.client.admin.command(
            {'setParameter': 1, 'internalQueryEnableCascadesOptimizer': state})

    def explain(self, collection_name: str, pipeline: Pipeline) -> dict[str, any]:
        """Return explain for the given pipeline."""
        return self.database.command(
            'explain', {'aggregate': collection_name, 'pipeline': pipeline, 'cursor': {}},
            verbosity='executionStats')

    def hide_index(self, collection_name: str, index_name: str) -> None:
        """Hide the given index from the query optimizer."""
        self.database.command(
            {'collMod': collection_name, 'index': {'name': index_name, 'hidden': True}})

    def unhide_index(self, collection_name: str, index_name: str) -> None:
        """Make the given index visible for the query optimizer."""
        self.database.command(
            {'collMod': collection_name, 'index': {'name': index_name, 'hidden': False}})

    def hide_all_indexes(self, collection_name: str) -> None:
        """Hide all indexes of the given collection from the query optimizer."""
        for index in self.database[collection_name].list_indexes():
            if index['name'] != '_id_':
                self.hide_index(collection_name, index['name'])

    def unhide_all_indexes(self, collection_name: str) -> None:
        """Make all indexes of the given collection visible fpr the query optimizer."""
        for index in self.database[collection_name].list_indexes():
            if index['name'] != '_id_':
                self.unhide_index(collection_name, index['name'])

    def drop_collection(self, collection_name: str) -> None:
        """Drop collection."""
        self.database[collection_name].drop()

    def insert_many(self, collection_name: str, docs: Sequence[Mapping[str, any]]) -> None:
        """Insert documents into the collection with the given name."""
        requests = [InsertOne(doc) for doc in docs]
        self.database[collection_name].bulk_write(requests, ordered=False)

    def get_all_documents(self, collection_name: str):
        """Get all documents from the collection with the given name."""
        return self.database[collection_name].find({})

    def get_stats(self, collection_name: str):
        """Get collection statistics."""
        return self.database.command('collstats', collection_name)

    def get_average_document_size(self, collection_name: str) -> float:
        """Get average document size for the given collection."""
        stats = self.get_stats(collection_name)
        avg_size = stats.get('avgObjSize')
        return avg_size if avg_size is not None else 0
