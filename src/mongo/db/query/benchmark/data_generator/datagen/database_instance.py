# Copyright (C) 2025-present MongoDB, Inc.
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

"""A wrapper with useful methods over MongoDB database."""

from __future__ import annotations

import shutil
import subprocess
from contextlib import asynccontextmanager
from dataclasses import dataclass
from enum import Enum, auto
from typing import Any, Mapping, NewType, Sequence

from pymongo import AsyncMongoClient

__all__ = ["DatabaseInstance", "Pipeline"]
"""MongoDB Aggregate's Pipeline"""
Pipeline = NewType("Pipeline", Sequence[Mapping[str, Any]])


@dataclass
class DatabaseConfig:
    """Database configuration."""

    connection_string: str
    database_name: str
    restore_from_dump: RestoreMode
    restore_additional_args: list


class RestoreMode(Enum):
    """Restore database from dump mode."""

    # Never restore database from dump.
    NEVER = auto()

    # Restore only new (non existing) database from dump
    ONLY_NEW = auto()

    # Always restore database from dump
    ALWAYS = auto()


class DatabaseInstance:
    """MongoDB Database wrapper."""

    def __init__(self, config: DatabaseConfig) -> None:
        """Initialize wrapper."""
        self.config = config
        self.client = AsyncMongoClient(config.connection_string)
        self.database = self.client[config.database_name]

    def __enter__(self):
        if self.config.restore_from_dump == RestoreMode.ALWAYS or (
            self.config.restore_from_dump == RestoreMode.ONLY_NEW
            and self.config.database_name not in self.client.list_database_names()
        ):
            self.restore()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        pass

    async def drop(self):
        """Drop the database."""
        await self.client.drop_database(self.config.database_name)

    def restore(self):
        """Restore the database."""

        if (mongorestore := shutil.which("mongorestore")) is None:
            raise RuntimeError("Cannot find a mongodump binary along PATH.")

        subprocess.run(
            [
                mongorestore,
                "--uri",
                self.config.connection_string,
            ]
            + self.config.restore_additional_args,
            shell=True,
            check=True,
        )

    def dump(self, additional_args: list):
        """Dump the database."""

        if (mongodump := shutil.which("mongodump")) is None:
            raise RuntimeError("Cannot find a mongodump binary along PATH.")

        subprocess.run(
            [
                mongodump,
                "--uri",
                self.config.connection_string,
                "--db",
                self.config.database_name,
            ]
            + additional_args,
            check=True,
        )

    async def analyze_field(self, collection_name: str, field: str, number_buckets: int = 1000) -> None:
        """
        Run 'analyze' on a given field.
        Analyze is currently not persisted across restarts, or when dumping or restoring.
        """
        await self.database.command({"analyze": collection_name, "key": field, "numberBuckets": number_buckets})

    async def set_parameter(self, name: str, value: any) -> None:
        """Set MongoDB Parameter."""
        await self.client.admin.command({"setParameter": 1, name: value})

    async def get_parameter(self, name: str) -> any:
        return (await self.client.admin.command({"getParameter": 1, name: 1}))[name]

    async def enable_sbe(self, state: bool) -> None:
        """Enable new query execution engine. Throw pymongo.errors.OperationFailure in case of failure."""
        await self.set_parameter(
            "internalQueryFrameworkControl",
            "trySbeEngine" if state else "forceClassicEngine",
        )

    async def explain(self, collection_name: str, pipeline: Pipeline) -> dict[str, any]:
        """Return explain for the given pipeline."""
        return await self.database.command(
            "explain",
            {"aggregate": collection_name, "pipeline": pipeline, "cursor": {}},
            verbosity="executionStats",
        )

    async def hide_index(self, collection_name: str, index_name: str) -> None:
        """Hide the given index from the query optimizer."""
        await self.database.command(
            {"collMod": collection_name, "index": {"name": index_name, "hidden": True}}
        )

    async def unhide_index(self, collection_name: str, index_name: str) -> None:
        """Make the given index visible for the query optimizer."""
        await self.database.command(
            {"collMod": collection_name, "index": {"name": index_name, "hidden": False}}
        )

    async def hide_all_indexes(self, collection_name: str) -> None:
        """Hide all indexes of the given collection from the query optimizer."""
        for index in self.database[collection_name].list_indexes():
            if index["name"] != "_id_":
                await self.hide_index(collection_name, index["name"])

    async def unhide_all_indexes(self, collection_name: str) -> None:
        """Make all indexes of the given collection visible fpr the query optimizer."""
        for index in self.database[collection_name].list_indexes():
            if index["name"] != "_id_":
                await self.unhide_index(collection_name, index["name"])

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
        return await self.database.command("collstats", collection_name)

    async def get_average_document_size(self, collection_name: str) -> float:
        """Get average document size for the given collection."""
        stats = await self.get_stats(collection_name)
        avg_size = stats.get("avgObjSize")
        return avg_size if avg_size is not None else 0


class DatabaseParameter:
    """A utility class to work with MongoDB parameters."""

    def __init__(self, database: DatabaseInstance, parameter_name: str) -> None:
        """Initialize the class."""
        self.database = database
        self.parameter_name = parameter_name
        self.original_value = None

    async def set(self, value):
        """Set the parameter's value."""
        await self.database.set_parameter(self.parameter_name, value)

    async def remember(self):
        """Store the current value of the parameter so it can be restored lately."""
        self.original_value = await self.database.get_parameter(self.parameter_name)

    async def restore(self):
        """Restore the remembered value of the parameter."""
        if self.original_value is not None:
            await self.set(self.original_value)
        else:
            raise ValueError(f'The parameter "{self.parameter_name}" has not been remembered.')


@asynccontextmanager
async def get_database_parameter(database: DatabaseInstance, parameter_name: str):
    """Create a new instance of a context manager on top of DatabaseParameter. It restores the original value on teardown. Useful when we need temporarily change a parameter."""
    param = DatabaseParameter(database, parameter_name)
    await param.remember()
    try:
        yield param
    finally:
        await param.restore()
