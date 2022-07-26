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
import time
import random
from typing import Sequence
import pymongo
from pymongo import InsertOne, IndexModel
from pymongo.collection import Collection
from common import timer_decorator
from config import DataGeneratorConfig, DataType
from database_instance import DatabaseInstance

__all__ = ['DataGenerator']


def coll_name(doc_count: int, data_type: DataType, field_count: int) -> str:
    """Generate collection name for the given parameters."""
    return f'c_{doc_count}_{str(data_type)}_{field_count}'


def field_name(pos: int) -> str:
    """Generate field name."""
    return f'f_{pos}'


def generate_fields(field_count: int) -> list[str]:
    """Generate list of field names."""
    return [field_name(i) for i in range(field_count)]


@dataclass
class FieldInfo:
    """Field-related information."""

    name: str
    type: DataType


@dataclass
class CollectionInfo:
    """Collection-related information."""

    name: str
    fields: Sequence[FieldInfo]
    documents_count: int


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
        coll_fields = [
            generate_fields(field_count) for field_count in config.collection_fields_counts
        ]

        self.collection_infos = list(self._generate_collection_infos(coll_fields))

        self.generators = {
            DataType.INTEGER: gen_random_digit, DataType.STRING: self.gen_random_string,
            DataType.ARRAY: gen_random_array
        }

    def populate_collections(self) -> None:
        """Create and populate collections for each combination of size and data type in the corresponding 'docCounts' and 'dataTypes' input arrays.

        All collections have the same schema defined by one of the elements of 'collFields'.
        """

        if not self.config.enabled:
            return

        self.database.enable_cascades(False)
        t0 = time.time()
        for coll_info in self.collection_infos:
            coll = self.database.database.get_collection(coll_info.name)
            coll.drop()
            self._populate_collection(coll, coll_info)
            create_single_field_indexes(coll, coll_info.fields)
            create_compound_index(coll, coll_info.fields)

        t1 = time.time()
        print(f'\npopulate Collections took {t1-t0} s.')

    def _generate_collection_infos(self, coll_fields: list[list[str]]):
        for field_names in coll_fields:
            for doc_count in self.config.collection_cardinalities:
                for data_type in self.config.data_types:
                    fields = [FieldInfo(name=fn, type=data_type) for fn in field_names]
                    name = coll_name(doc_count, data_type, len(fields))
                    yield CollectionInfo(name=name, fields=fields, documents_count=doc_count)

    @timer_decorator
    def _populate_collection(self, coll: Collection, coll_info: CollectionInfo) -> None:
        print(f'\nGenerating ${coll_info.name} ...')
        batch_size = self.config.batch_size
        for _ in range(coll_info.documents_count // batch_size):
            self._populate_batch(coll, batch_size, coll_info.fields)
        if coll_info.documents_count % batch_size > 0:
            self._populate_batch(coll, coll_info.documents_count % batch_size, coll_info.fields)

    def _populate_batch(self, coll: Collection, documents_count: int,
                        fields: Sequence[FieldInfo]) -> None:
        requests = [
            InsertOne(doc) for doc in self._generate_collection_data(documents_count, fields)
        ]
        coll.bulk_write(requests, ordered=False)

    def _generate_collection_data(self, documents_count: int, fields: Sequence[FieldInfo]):
        documents = [{} for _ in range(documents_count)]
        for field in fields:
            for field_index, field_data in enumerate(
                    self._generate_random_data(field.type, documents_count)):
                documents[field_index][field.name] = field_data
        return documents

    def gen_random_string(self) -> str:
        """Generate random string."""
        return f'{gen_random_digit()}{gen_random_digit()}{"x"*(self.config.string_length-2)}'

    def _generate_random_data(self, data_type: DataType, count: int):
        generator = self.generators.get(data_type)
        if generator is None:
            raise ValueError(f'Unknown dataType {data_type}')
        return [generator() for _ in range(count)]


def create_single_field_indexes(coll: Collection, fields: Sequence[FieldInfo]) -> None:
    """Create single-fields indexes on the given collection."""

    t0 = time.time()

    indexes = [IndexModel([(field.name, pymongo.ASCENDING)]) for field in fields]
    coll.create_indexes(indexes)

    t1 = time.time()
    print(f'createSingleFieldIndexes took {t1 - t0} s.')


def create_compound_index(coll: Collection, fields: Sequence[FieldInfo]) -> None:
    """Create a coumpound index on the given collection."""

    field_names = [fi.name for fi in fields if fi.type != DataType.ARRAY]
    if len(field_names) < 2:
        print(f'Collection: {coll.name} not suitable for compound index')
        return

    t0 = time.time()

    index_spec = [(field, pymongo.ASCENDING) for field in field_names]
    coll.create_index(index_spec)

    t1 = time.time()
    print(f'createCompoundIndex took {t1 - t0} s.')


def gen_random_digit() -> int:
    """Generate random digit."""
    return random.randint(0, 9)


def gen_random_array(array_size: int = 10) -> list[dict[str, int]]:
    """Generate random array of objects."""

    def gen_element(index: int) -> dict[str, int]:
        return dict([(field_name(index), gen_random_digit())])

    return [gen_element(j) for j in range(array_size)]
