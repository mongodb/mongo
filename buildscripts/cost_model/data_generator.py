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
import time
import random
import pymongo
from pymongo import InsertOne, IndexModel
from pymongo.collection import Collection
from config import DataGeneratorConfig
from database_instance import DatabaseInstance

__all__ = ['DataGenerator']


def coll_name(doc_count: int, data_type: str, field_count: int) -> str:
    """Generate collection name for the given parameters."""
    return f'c_{doc_count}_{data_type}_{field_count}'


def field_name(pos: int) -> str:
    """Generate field name."""
    return f'f_{pos}'


def generate_fields(field_count: int) -> list[str]:
    """Generate list of field names."""
    return [field_name(i) for i in range(field_count)]


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
        self.coll_fields = [
            generate_fields(field_count) for field_count in config.collection_fields_counts
        ]

    def populate_collections(self) -> None:
        """Create and populate collections for each combination of size and data type in the corresponding 'docCounts' and 'dataTypes' input arrays.

        All collections have the same schema defined by one of the elements of 'collFields'.
        """

        if not self.config.enabled:
            return

        self.database.enable_cascades(False)
        t0 = time.time()
        for fields in self.coll_fields:
            for doc_count in self.config.collection_cardinalities:
                for data_type in self.config.data_types:
                    coll = self.database.database.get_collection(
                        coll_name(doc_count, data_type, len(fields)))
                    coll.drop()
                    self._populate_collection(coll, doc_count, fields, data_type)
                    create_single_field_indexes(coll, fields)
                    if len(fields) > 1:
                        create_compound_index(coll, fields)
        t1 = time.time()
        print(f'\npopulate Collections took {t1-t0} s.')

    def list_collection_names(self):
        """Generate collections names for the configured fileds, number of documents and types in the collection."""

        for fields in self.coll_fields:
            for doc_count in self.config.collection_cardinalities:
                for data_type in self.config.data_types:
                    yield coll_name(doc_count, data_type, len(fields))

    def _populate_collection(self, coll: Collection, doc_count: int, fields: list[str],
                             data_type: str) -> None:
        print(f'\nGenerating ${coll.name} ...')
        start_time = time.time()
        requests = [InsertOne(self._gen_random_doc(fields, data_type)) for _ in range(doc_count)]
        generate_time = time.time()
        coll.bulk_write(requests, ordered=False)
        finish_time = time.time()
        print(
            f'Total time: {finish_time - start_time} s., generate time: {generate_time - start_time} s., insert time: {finish_time - generate_time} s.'
        )

    def gen_random_string(self) -> str:
        """Generate random string."""
        return f'{gen_random_digit()}{gen_random_digit()}{"x"*(self.config.string_length-2)}'

    def _gen_random_doc(self, doc_fields: list[str], data_type: str) -> dict[str, any]:
        generators = {
            'int': gen_random_digit, 'str': self.gen_random_string, 'arr': gen_random_array
        }

        generator = generators.get(data_type)
        if generator is None:
            raise ValueError(f'Unknown dataType {data_type}')

        return {field: generator() for field in doc_fields}


def create_single_field_indexes(coll: Collection, fields: list[str]) -> None:
    """Create single-fields indexes on the given collection."""

    t0 = time.time()

    indexes = [IndexModel([(field, pymongo.ASCENDING)]) for field in fields]
    coll.create_indexes(indexes)

    t1 = time.time()
    print(f'createSingleFieldIndexes took {t1 - t0} s.')


def create_compound_index(coll: Collection, fields: list[str]) -> None:
    """Create a coumpound index on the given collection."""

    if not (coll.name.startswith('c_') or coll.name.startswith('c2_')) or 'arr_' in coll.name:
        print(f'Collection: {coll.name} not suitable for compound index')
        return

    t0 = time.time()

    index_spec = [(field, pymongo.ASCENDING) for field in fields]
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
