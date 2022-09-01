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

from __future__ import annotations
from dataclasses import dataclass
from enum import Enum
from typing import Sequence
from random_generator import RandomDistribution


@dataclass
class Config:
    """Main configuration class."""

    database: DatabaseConfig
    data_generator: DataGeneratorConfig
    abt_calibrator: AbtCalibratorConfig
    workload_execution: WorkloadExecutionConfig


@dataclass
class DatabaseConfig:
    """Database configuration."""

    connection_string: str
    database_name: str
    dump_path: str
    restore_from_dump: RestoreMode
    dump_on_exit: bool


class RestoreMode(Enum):
    """Restore database from dump mode."""

    # Never restore database from dump.
    NEVER = 0

    # Restore only new (non existing) database from dump
    ONLY_NEW = 1

    # Always restore database from dump
    ALWAYS = 2


@dataclass
class DataGeneratorConfig:
    """Data Generator configuration."""

    enabled: bool
    collection_cardinalities: list[int]
    collection_templates: list[CollectionTemplate]
    batch_size: int


@dataclass
class CollectionTemplate:
    """Collection template used to generate a collection with random data."""

    name: str
    fields: Sequence[FieldTemplate]
    compound_indexes: Sequence[Sequence[str]]


@dataclass
class FieldTemplate:
    """Field template used to generate a collection with random data."""

    name: str
    data_type: DataType
    distribution: RandomDistribution
    indexed: bool


class DataType(Enum):
    """Data types."""

    INTEGER = 0
    STRING = 1
    ARRAY = 2

    def __str__(self):
        return self.name.lower()[:3]


@dataclass
class AbtCalibratorConfig:
    """ABT Calibrator configuration."""

    enabled: bool
    # Share of data used for testing the model. Usually it should be around 0.1-0.3.
    test_size: float
    input_collection_name: str
    trace: bool


class WriteMode(Enum):
    """Write mode enum."""

    APPEND = 0
    REPLACE = 1


@dataclass
class WorkloadExecutionConfig:
    """Workload execution configuration."""

    enabled: bool
    output_collection_name: str
    write_mode: WriteMode
    warmup_runs: int
    runs: int
