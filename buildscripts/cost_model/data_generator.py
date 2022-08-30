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
"""Implements to populate MongoDB collections with generated data used to calibrate Cost Model."""

from __future__ import annotations
from dataclasses import dataclass
from importlib.metadata import distribution
import time
import random
from typing import Sequence
import asyncio
import pymongo
from pymongo import IndexModel
from motor.motor_asyncio import AsyncIOMotorCollection
from motor.motor_asyncio import AsyncIOMotorDatabase
from random_generator import RandomDistribution
from config import DataGeneratorConfig, DataType
from database_instance import DatabaseInstance
from random_generator_config import distributions

__all__ = ['DataGenerator']


@dataclass
class FieldInfo:
    """Field-related information."""

    name: str
    type: DataType
    distribution: RandomDistribution
    indexed: bool


@dataclass
class CollectionInfo:
    """Collection-related information."""

    name: str
    fields: Sequence[FieldInfo]
    documents_count: int
    compound_indexes: Sequence[Sequence[str]]


class DataGenerator:
    """Create and populate collections with generated data."""

    def __init__(self, database: DatabaseInstance, config: DataGeneratorConfig):
        """Create new DataGenerator.

        Keyword Arguments:
        database -- Instance of Database object
        stringlength -- Length of generated strings
        """

        self.database = database
        self.config = config

        self.collection_infos = list(self._generate_collection_infos())

    async def populate_collections(self) -> None:
        """Create and populate collections for each combination of size and data type in the corresponding 'docCounts' and 'dataTypes' input arrays.

        All collections have the same schema defined by one of the elements of 'collFields'.
        """

        if not self.config.enabled:
            return

        await self.database.enable_cascades(False)
        t0 = time.time()
        tasks = []
        for coll_info in self.collection_infos:
            coll = self.database.database[coll_info.name]
            await coll.drop()
            tasks.append(asyncio.create_task(self._populate_collection(coll, coll_info)))
            tasks.append(asyncio.create_task(create_single_field_indexes(coll, coll_info.fields)))
            tasks.append(asyncio.create_task(create_compound_indexes(coll, coll_info)))

        for task in tasks:
            await task

        t1 = time.time()
        print(f'\npopulate Collections took {t1-t0} s.')

    def _generate_collection_infos(self):
        for coll_template in self.config.collection_templates:
            fields = [
                FieldInfo(name=ft.name, type=ft.data_type,
                          distribution=distributions[ft.distribution], indexed=ft.indexed)
                for ft in coll_template.fields
            ]
            for doc_count in self.config.collection_cardinalities:
                name = f'{coll_template.name}_{doc_count}'
                yield CollectionInfo(name=name, fields=fields, documents_count=doc_count,
                                     compound_indexes=coll_template.compound_indexes)

    async def _populate_collection(self, coll: AsyncIOMotorCollection,
                                   coll_info: CollectionInfo) -> None:
        print(f'\nGenerating ${coll_info.name} ...')
        batch_size = self.config.batch_size
        tasks = []
        for _ in range(coll_info.documents_count // batch_size):
            tasks.append(asyncio.create_task(populate_batch(coll, batch_size, coll_info.fields)))
        if coll_info.documents_count % batch_size > 0:
            tasks.append(
                asyncio.create_task(
                    populate_batch(coll, coll_info.documents_count % batch_size, coll_info.fields)))

        for task in tasks:
            await task


async def populate_batch(coll: AsyncIOMotorCollection, documents_count: int,
                         fields: Sequence[FieldInfo]) -> None:
    """Generate collection data and write it to the collection."""

    await coll.insert_many(generate_collection_data(documents_count, fields), ordered=False)


def generate_collection_data(documents_count: int, fields: Sequence[FieldInfo]):
    """Generate random data for the specified fields of a collection."""

    documents = [{} for _ in range(documents_count)]
    for field in fields:
        for field_index, field_data in enumerate(field.distribution.generate(documents_count)):
            documents[field_index][field.name] = field_data
    return documents


async def create_single_field_indexes(coll: AsyncIOMotorCollection,
                                      fields: Sequence[FieldInfo]) -> None:
    """Create single-fields indexes on the given collection."""

    indexes = [IndexModel([(field.name, pymongo.ASCENDING)]) for field in fields if field.indexed]
    if len(indexes) > 0:
        await coll.create_indexes(indexes)

    index_spec = [(field.name, pymongo.ASCENDING) for field in fields]

    print(f'create_single_field_indexes done. {index_spec}')


async def create_compound_indexes(coll: AsyncIOMotorCollection, coll_info: CollectionInfo) -> None:
    """Create a coumpound indexes on the given collection."""

    indexes_spec = []
    index_specs = []
    for compound_index in coll_info.compound_indexes:
        index_spec = IndexModel([(field, pymongo.ASCENDING) for field in compound_index])
        indexes_spec.append(index_spec)
        index_specs.append([(field, pymongo.ASCENDING) for field in compound_index])
    if len(indexes_spec) > 0:
        await coll.create_indexes(indexes_spec)

    print(f'createCompoundIndex done. {index_specs}')
