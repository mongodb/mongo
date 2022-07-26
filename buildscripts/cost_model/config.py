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
import os
from typing import Mapping


@dataclass
class Config:
    """Main configuration class."""

    database: DatabaseConfig
    data_generator: DataGeneratorConfig
    abt_calibrator: AbtCalibratorConfig
    workload_execution: WorkloadExecutionConfig

    @staticmethod
    def create(json_config: dict[str, any]) -> Config:
        """Create new configuration object from JSON."""

        database = DatabaseConfig.create(json_config.get('database'))
        data_generator = DataGeneratorConfig.create(json_config.get('dataGenerator'))
        abt_calibrator = AbtCalibratorConfig.create(json_config.get('abtCalibrator'))
        workload_execution = WorkloadExecutionConfig.create(json_config.get("workloadExecution"))

        return Config(database=database, data_generator=data_generator,
                      abt_calibrator=abt_calibrator, workload_execution=workload_execution)


@dataclass
class DatabaseConfig:
    """Database configuration."""

    connection_string: str
    database_name: str
    dump_path: str
    restore_from_dump: RestoreMode
    dump_on_exit: bool

    @staticmethod
    def create(json_config: dict[str, any]) -> DatabaseConfig:
        """Create new configuration object from JSON."""

        default = DatabaseConfig(connection_string='mongodb://localhost', database_name='test',
                                 dump_path='/data/dump', restore_from_dump=RestoreMode.NEVER,
                                 dump_on_exit=False)
        if json_config is None:
            return default

        connection_string = json_config.get('connectionString', default.connection_string)
        database_name = json_config.get('databaseName', default.database_name)
        dump_path = process_path(json_config.get('dumpPath', default.dump_path))

        restore_from_dump_str = json_config.get('restoreFromDump', str(default.dump_path)).lower()
        if restore_from_dump_str == "never":
            restore_from_dump = RestoreMode.NEVER
        elif restore_from_dump_str == "onlynew":
            restore_from_dump = RestoreMode.ONLY_NEW
        elif restore_from_dump_str == "always":
            restore_from_dump = RestoreMode.ALWAYS
        else:
            raise ValueError("restoreFromDump must be equal to 'never', 'onlyNew', or 'aways'")

        dump_on_exit = json_config.get('dumpOnExit', default.dump_path)
        return DatabaseConfig(connection_string=connection_string, database_name=database_name,
                              dump_path=dump_path, restore_from_dump=restore_from_dump,
                              dump_on_exit=dump_on_exit)


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
    string_length: int
    collection_cardinalities: list[int]
    collection_fields_counts: list[int]
    data_types: list[DataType]
    batch_size: int

    @staticmethod
    def create(json_config: dict[str, any]) -> DataGeneratorConfig:
        """Create new configuration object from JSON."""

        default = DataGeneratorConfig(enabled=False, string_length=8, collection_cardinalities=[],
                                      collection_fields_counts=[], data_types=[], batch_size=10000)
        if json_config is None:
            return default

        enabled = json_config.get('enabled', default.enabled)
        string_length = json_config.get('stringLength', default.string_length)
        collection_cardinalities = json_config.get('collectionCardinalities',
                                                   default.collection_cardinalities)
        collection_fields_count = json_config.get('collectionFieldsCounts',
                                                  default.collection_fields_counts)
        data_types_str = json_config.get('dataTypes', default.data_types)
        data_types = [DataType.parse(dt, 'dataTypes') for dt in data_types_str]
        batch_size = json_config.get('batchSize', default.batch_size)
        return DataGeneratorConfig(enabled=enabled, string_length=string_length,
                                   collection_cardinalities=collection_cardinalities,
                                   collection_fields_counts=collection_fields_count,
                                   data_types=data_types, batch_size=batch_size)


class DataType(Enum):
    """Data types."""

    INTEGER = 0
    STRING = 1
    ARRAY = 2

    def __str__(self):
        return self.name.lower()[:3]

    @staticmethod
    def parse(type_str: str, field_name: str) -> DataType:
        """Parse DataType."""
        str_to_type = {'int': DataType.INTEGER, 'str': DataType.STRING, 'arr': DataType.ARRAY}

        return parse_multi_value(str_to_type, type_str, field_name)


@dataclass
class AbtCalibratorConfig:
    """ABT Calibrator configuration."""

    enabled: bool
    # Share of data used for testing the model. Usually it should be around 0.1-0.3.
    test_size: float
    input_collection_name: str
    trace: bool

    @staticmethod
    def create(json_config: dict[str, any]) -> AbtCalibratorConfig:
        """Create new configuration object from JSON."""

        default = AbtCalibratorConfig(enabled=False, test_size=0.2,
                                      input_collection_name='explains', trace=False)
        if json_config is None:
            return default

        enabled = json_config.get('enabled', default.enabled)
        test_size = json_config.get("testSize", default.test_size)
        if test_size <= 0.0 or test_size >= 1.0:
            raise ValueError('testSize must be greater than 0 and less than 1')
        input_collection_name = json_config.get('inputCollectionName',
                                                default.input_collection_name)
        trace = json_config.get('trace', default.trace)

        return AbtCalibratorConfig(enabled=enabled, test_size=test_size,
                                   input_collection_name=input_collection_name, trace=trace)


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

    @staticmethod
    def create(json_config: dict[str, any] | None) -> WorkloadExecutionConfig:
        """Create new configuration object from JSON."""

        default = WorkloadExecutionConfig(enabled=False, output_collection_name='explains',
                                          write_mode=WriteMode.APPEND, warmup_runs=1, runs=1)
        if json_config is None:
            return default

        enabled = json_config.get('enabled', default.enabled)
        output_collection_name = json_config.get('outputCollectionName',
                                                 default.output_collection_name)
        write_mode_str = json_config.get('writeMode', 'append').lower()
        if write_mode_str == 'append':
            write_mode = WriteMode.APPEND
        elif write_mode_str == 'replace':
            write_mode = WriteMode.REPLACE
        else:
            raise ValueError("writeMode must be equal to 'append' or 'replace'")

        runs = json_config.get('runs', default.runs)
        warmup_runs = json_config.get('warmupRuns', default.warmup_runs)

        return WorkloadExecutionConfig(enabled=enabled,
                                       output_collection_name=output_collection_name,
                                       write_mode=write_mode, warmup_runs=warmup_runs, runs=runs)


def process_path(path):
    """Expand user's home folder and convert to absolute path."""
    return os.path.abspath(os.path.expanduser(path))


def parse_multi_value(from_str_dict: Mapping[str, any], value_str: str, field_name: str) -> any:
    """Parse a string which may contain one of the predefined in from_str_dict values."""
    value = from_str_dict.get(value_str)
    if value is None:
        raise ValueError(
            f"{field_name} got {value_str} but must be equal to one of: {', '.join(from_str_dict.keys())}"
        )
    return value
