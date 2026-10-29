# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Configuration of Cost Model Calibration."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, Sequence

import pandas as pd
from random_generator import DataType, RandomDistribution


@dataclass
class Config:
    """Main configuration class."""

    database: DatabaseConfig
    data_generator: DataGeneratorConfig
    qs_calibrator: QuerySolutionCalibrationConfig
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
    create_indexes: bool
    collection_templates: list[CollectionTemplate]
    collection_name_with_card: bool
    write_mode: WriteMode
    batch_size: int


@dataclass
class CollectionTemplate:
    """Collection template used to generate a collection with random data."""

    name: str
    fields: Sequence[FieldTemplate]
    compound_indexes: Sequence[Sequence[str]]
    cardinalities: Sequence[int]


@dataclass
class FieldTemplate:
    """Field template used to generate a collection with random data."""

    name: str
    data_type: DataType
    distribution: RandomDistribution
    indexed: bool


@dataclass
class QsNodeCalibrationConfig:
    type: str
    filter_function: Callable[[Any], Any] = None
    variables_override: Callable[[pd.DataFrame], pd.DataFrame] = None
    # The difference between `name` and `type` is that `name` is a unique identifier for whatever is being calibrated,
    # while `type` is the QSN we will be measuring. These two may not always align, as in the case of
    # forward/backward scans or the different sort varieties (which are the same node but different calibrations).
    name: str = None


@dataclass
class QuerySolutionCalibrationConfig:
    """Query Solution Calibration configuration."""

    enabled: bool
    # Share of data used for testing the model. Usually it should be around 0.1-0.3.
    test_size: float
    input_collection_name: str
    trace: bool
    nodes: Sequence[QsNodeCalibrationConfig]


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
