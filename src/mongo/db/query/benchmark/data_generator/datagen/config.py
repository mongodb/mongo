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
"""Configuration of Cost Model Calibration."""

import dataclasses
import enum
import typing

import datagen.random

# TODO(SERVER-106819: integrate this file with the rest of the data generator.


@dataclasses.dataclass
class FieldTemplate:
    """Field template used to generate a collection with random data."""

    name: str
    data_type: datagen.random.DataType
    distribution: datagen.random.RandomDistribution
    indexed: bool


class WriteMode(enum.Enum):
    """Write mode enum."""

    APPEND = enum.auto()
    REPLACE = enum.auto()


@dataclasses.dataclass
class CollectionTemplate:
    """Collection template used to generate a collection with random data."""

    name: str
    fields: typing.Sequence[FieldTemplate]
    compound_indexes: typing.Sequence[typing.Sequence[str]]
    cardinalities: typing.Sequence[int]


@dataclasses.dataclass
class DataGeneratorConfig:
    """Data Generator configuration."""

    enabled: bool
    create_indexes: bool
    collection_templates: list[CollectionTemplate]
    collection_name_with_card: bool
    write_mode: WriteMode
    batch_size: int


@dataclasses.dataclass
class DataGeneratorProducer:
    generator_function: typing.Callable
