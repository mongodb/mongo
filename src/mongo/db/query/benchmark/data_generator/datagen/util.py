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

"""Utilities for data generation."""

import asyncio
import collections
import contextlib
import dataclasses
import enum
import time
import typing

import datagen.config
import datagen.faker
import datagen.random
import datagen.statistics
import faker
import pymongo

####################################################################################################
#
# Correlated data generator definitions begin here.
#
####################################################################################################


@dataclasses.dataclass
class CorrelatedContext(contextlib.ContextDecorator):
    """This is a context that makes it easier to implement correlated data. For example,
    with CorrelatedContext(self.generator, 'a context key'):
        ...

    is equivalent to

    self.generator.recall('a context key')
    ...
    self.generator.reset()

    but with a lower likelihood of forgetting the `.reset()`.
    """

    resource: datagen.faker.CorrelatedGenerator | datagen.random.CorrelatedRng
    name: str | None

    def __enter__(self):
        if self.name:
            self.resource.recall(self.name)

    def __exit__(self, *_):
        if self.name:
            self.resource.reset()


class CorrelatedDataFactory:
    """This is a factory class for producing randomly-generated correlated data."""

    def __init__(self, provider: faker.providers.BaseProvider, fkr: faker.proxy.Faker):
        self.faker = fkr
        self.provider = provider
        self.random = provider.generator.random
        self.statistics = datagen.statistics.StatisticsRegister()

    def build(self, obj_type: type):
        """Produce a randomly-generated value of the desired type."""
        hints = typing.get_type_hints(obj_type)
        if hints:
            fields = {}
            queue = []

            def queue_items():
                for k, v in hints.items():
                    yield k, v
                for k, v in queue:
                    yield k, v

            for field, hint in queue_items():
                with self.statistics.path_cm(field):
                    dependencies = {}
                    if isinstance(hint, Specification):
                        if any(dependency not in fields for dependency in hint.dependson):
                            queue.append((field, hint))
                            continue
                        dependencies = {
                            dependency: fields[dependency] for dependency in hint.dependson
                        }
                    field_value = self.build_using_hint(obj_type, field, hint, dependencies)
                    fields[field] = field_value
            self.statistics.register_fields(fields)
            return obj_type(**fields)
        # From here, generate some basic types by default.
        elif issubclass(obj_type, enum.Enum):
            return self.provider.enum(obj_type)
        elif hasattr(obj_type, "__args__"):
            return getattr(self.provider, f"py{obj_type.__name__}")(value_types=obj_type.__args__)
        else:
            return getattr(self.provider, f"py{obj_type.__name__}")()

    def build_using_hint(self, obj_type: type, field: str, hint, dependencies):
        """Wrap around build() to make use of type hints in the specification."""
        if isinstance(hint, Specification):
            with CorrelatedContext(self.random, hint.correlation):
                if hint.source and hasattr(obj_type, f"make_{field}"):
                    raise RuntimeError(
                        f"Field {field} has both a hint source and a make function, but should only have one."
                    )
                elif hint.source:
                    # A source attribute in the specification overrides any make functions.
                    return hint.source(
                        self.faker,
                        **dependencies,
                    )
                elif hasattr(obj_type, f"make_{field}"):
                    # This is another way to specify custom generation logic.
                    return getattr(obj_type, f"make_{field}")(
                        self.faker,
                        **dependencies,
                    )
                else:
                    return self.build(hint.type)
        else:
            return self.build(hint)


####################################################################################################
#
# Cost model calibration data generator definitions begin here.
#
####################################################################################################


@dataclasses.dataclass
class FieldInfo:
    """Field-related information."""

    name: str
    type: datagen.random.DataType
    distribution: datagen.random.RandomDistribution
    indexed: bool


@dataclasses.dataclass
class CollectionInfo:
    """Collection-related information."""

    name: str
    fields: typing.Sequence[FieldInfo]
    documents_count: int
    compound_indexes: typing.Sequence[typing.Sequence[str]]


class DataGenerator:
    """Create and populate collections with generated data."""

    def __init__(
        self,
        database: datagen.database_instance.DatabaseInstance,
        config: datagen.config.DataGeneratorConfig,
    ):
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

        t0 = time.time()
        tasks = []
        for coll_info in self.collection_infos:
            coll = self.database.database[coll_info.name]
            if self.config.write_mode == datagen.config.WriteMode.REPLACE:
                await coll.drop()
            tasks.append(asyncio.create_task(self._populate_collection(coll, coll_info)))
            if self.config.create_indexes:
                tasks.append(
                    asyncio.create_task(create_single_field_indexes(coll, coll_info.fields))
                )
                tasks.append(asyncio.create_task(create_compound_indexes(coll, coll_info)))

        for task in tasks:
            await task

        t1 = time.time()
        print(f"\npopulate Collections took {t1 - t0} s.")

    def _generate_collection_infos(self):
        for coll_template in self.config.collection_templates:
            fields = [
                FieldInfo(
                    name=ft.name,
                    type=ft.data_type,
                    distribution=ft.distribution,
                    indexed=ft.indexed,
                )
                for ft in coll_template.fields
            ]
            for doc_count in coll_template.cardinalities:
                name = f"{coll_template.name}"
                if self.config.collection_name_with_card is True:
                    name = f"{coll_template.name}_{doc_count}"
                yield CollectionInfo(
                    name=name,
                    fields=fields,
                    documents_count=doc_count,
                    compound_indexes=coll_template.compound_indexes,
                )

    async def _populate_collection(
        self, coll: pymongo.asynchronous.collection.AsyncCollection, coll_info: CollectionInfo
    ) -> None:
        print(f"\nGenerating ${coll_info.name} ...")
        batch_size = self.config.batch_size
        tasks = []
        for _ in range(coll_info.documents_count // batch_size):
            tasks.append(asyncio.create_task(populate_batch(coll, batch_size, coll_info.fields)))
        if coll_info.documents_count % batch_size > 0:
            tasks.append(
                asyncio.create_task(
                    populate_batch(coll, coll_info.documents_count % batch_size, coll_info.fields)
                )
            )

        for task in tasks:
            await task


async def populate_batch(
    coll: pymongo.asynchronous.collection.AsyncCollection,
    documents_count: int,
    fields: typing.Sequence[FieldInfo],
) -> None:
    """Generate collection data and write it to the collection."""

    await coll.insert_many(generate_collection_data(documents_count, fields), ordered=False)


def generate_collection_data(documents_count: int, fields: typing.Sequence[FieldInfo]):
    """Generate random data for the specified fields of a collection."""

    documents = [{} for _ in range(documents_count)]
    for field in fields:
        for field_index, field_data in enumerate(field.distribution.generate(documents_count)):
            documents[field_index][field.name] = field_data
    return documents


async def create_single_field_indexes(
    coll: pymongo.asynchronous.collection.AsyncCollection, fields: typing.Sequence[FieldInfo]
) -> None:
    """Create single-fields indexes on the given collection."""

    indexes = [
        pymongo.IndexModel([(field.name, pymongo.ASCENDING)]) for field in fields if field.indexed
    ]
    if len(indexes) > 0:
        await coll.create_indexes(indexes)
        print(f"create_single_field_indexes done. {[index.document for index in indexes]}")


async def create_compound_indexes(
    coll: pymongo.asynchronous.collection.AsyncCollection, coll_info: CollectionInfo
) -> None:
    """Create a coumpound indexes on the given collection."""

    indexes_spec = []
    index_specs = []
    for compound_index in coll_info.compound_indexes:
        index_spec = pymongo.IndexModel([(field, pymongo.ASCENDING) for field in compound_index])
        indexes_spec.append(index_spec)
        index_specs.append([(field, pymongo.ASCENDING) for field in compound_index])
    if len(indexes_spec) > 0:
        await coll.create_indexes(indexes_spec)
        print(f"create_compound_indexes done. {index_specs}")


class SpecialValue(enum.Enum):
    MISSING = enum.auto()


MISSING = SpecialValue.MISSING


@dataclasses.dataclass
class Specification:
    type: type
    correlation: str | None = None
    distribution: str = "default"
    source: collections.abc.Callable[[faker.proxy.Faker], any] | None = None
    dependson: tuple[str] = ()

    def __repr__(self):
        specs = (
            f"{attr}: {getattr(self, attr)}"
            for attr in ("correlation", "distribution", "dependson")
            # Print if the value is not None. Also ignore zero-length tuples.
            if getattr(self, attr) is not None
            and (not isinstance(getattr(self, attr), tuple) or getattr(self, attr))
        )
        return f"{self.type.__name__}({', '.join(specs)})"
