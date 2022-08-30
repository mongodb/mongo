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
from typing import Sequence, Mapping, NewType, Any
import subprocess
from motor.motor_asyncio import AsyncIOMotorClient
from config import DatabaseConfig, RestoreMode

__all__ = ['DatabaseInstance', 'Pipeline']
"""MongoDB Aggregate's Pipeline"""
Pipeline = NewType('Pipeline', Sequence[Mapping[str, Any]])


class DatabaseInstance:
    """MongoDB Database wrapper."""

    def __init__(self, config: DatabaseConfig) -> None:
        """Initialize wrapper."""
        self.config = config
        self.client = AsyncIOMotorClient(config.connection_string)
        self.database = self.client[config.database_name]

    def __enter__(self):
        if self.config.restore_from_dump == RestoreMode.ALWAYS or (
                self.config.restore_from_dump == RestoreMode.ONLY_NEW
                and self.config.database_name not in self.client.list_database_names()):
            self.restore()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        if self.config.dump_on_exit:
            self.enable_cascades(False)
            self.dump()

    async def drop(self):
        """Drop the database."""
        await self.client.drop_database(self.config.database_name)

    def restore(self):
        """Restore the database from the 'self.dump_directory'."""
        subprocess.run(['mongorestore', '--nsInclude', f'{self.config.database_name}.*', '--drop'],
                       shell=True, check=True, cwd=self.config.dump_path)

    def dump(self):
        """Dump the database into 'self.dump_directory'."""
        subprocess.run(['mongodump', '--db', self.config.database_name], cwd=self.config.dump_path,
                       check=True)

    async def enable_sbe(self, state: bool) -> None:
        """Enable new query execution engine. Throw pymongo.errors.OperationFailure in case of failure."""
        await self.client.admin.command({
            'setParameter': 1,
            'internalQueryFrameworkControl': 'trySbeEngine' if state else 'forceClassicEngine'
        })

    async def enable_cascades(self, state: bool) -> None:
        """Enable new query optimizer. Requires featureFlagCommonQueryFramework set to True."""
        await self.client.admin.command(
            {'configureFailPoint': 'enableExplainInBonsai', 'mode': 'alwaysOn'})
        await self.client.admin.command({
            'setParameter': 1,
            'internalQueryFrameworkControl': 'tryBonsai' if state else 'trySbeEngine'
        })

    async def explain(self, collection_name: str, pipeline: Pipeline) -> dict[str, any]:
        """Return explain for the given pipeline."""
        return await self.database.command(
            'explain', {'aggregate': collection_name, 'pipeline': pipeline, 'cursor': {}},
            verbosity='executionStats')

    async def hide_index(self, collection_name: str, index_name: str) -> None:
        """Hide the given index from the query optimizer."""
        await self.database.command(
            {'collMod': collection_name, 'index': {'name': index_name, 'hidden': True}})

    async def unhide_index(self, collection_name: str, index_name: str) -> None:
        """Make the given index visible for the query optimizer."""
        await self.database.command(
            {'collMod': collection_name, 'index': {'name': index_name, 'hidden': False}})

    async def hide_all_indexes(self, collection_name: str) -> None:
        """Hide all indexes of the given collection from the query optimizer."""
        for index in self.database[collection_name].list_indexes():
            if index['name'] != '_id_':
                await self.hide_index(collection_name, index['name'])

    async def unhide_all_indexes(self, collection_name: str) -> None:
        """Make all indexes of the given collection visible fpr the query optimizer."""
        for index in self.database[collection_name].list_indexes():
            if index['name'] != '_id_':
                await self.unhide_index(collection_name, index['name'])

    async def drop_collection(self, collection_name: str) -> None:
        """Drop collection."""
        await self.database[collection_name].drop()

    async def insert_many(self, collection_name: str, docs: Sequence[Mapping[str, any]]) -> None:
        """Insert documents into the collection with the given name."""
        await self.database[collection_name].insert_many(docs, ordered=False)

    async def get_all_documents(self, collection_name: str):
        """Get all documents from the collection with the given name."""
        return await self.database[collection_name].find({}).to_list(length=None)

    async def get_stats(self, collection_name: str):
        """Get collection statistics."""
        return await self.database.command('collstats', collection_name)

    async def get_average_document_size(self, collection_name: str) -> float:
        """Get average document size for the given collection."""
        stats = await self.get_stats(collection_name)
        avg_size = stats.get('avgObjSize')
        return avg_size if avg_size is not None else 0
