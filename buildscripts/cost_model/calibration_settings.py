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
"""Calibration configuration."""

import config
from random_generator import RangeGenerator, DataType, RandomDistribution

__all__ = ['main_config', 'distributions']

# Data distributions settings.
distributions = {}

string_choice_values = [
    'h',
    'hi',
    'hi!',
    'hola',
    'hello',
    'square',
    'squared',
    'gaussian',
    'chisquare',
    'chisquared',
    'hello world',
    'distribution',
]

string_choice_weights = [10, 20, 5, 17, 30, 7, 9, 15, 40, 2, 12, 1]

distributions['string_choice'] = RandomDistribution.choice(string_choice_values,
                                                           string_choice_weights)

string_range_4 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "abca", "abc_"))
string_range_5 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "abcda", "abcd_"))
string_range_7 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "hello_a", "hello__"))
string_range_12 = RandomDistribution.normal(
    RangeGenerator(DataType.STRING, "helloworldaa", "helloworldd_"))

distributions['string_mixed'] = RandomDistribution.mixed(
    [string_range_4, string_range_5, string_range_7, string_range_12], [0.1, 0.15, 0.25, 0.5])

distributions['string_uniform'] = RandomDistribution.uniform(
    RangeGenerator(DataType.STRING, "helloworldaa", "helloworldd_"))

# Database settings
database = config.DatabaseConfig(connection_string='mongodb://localhost',
                                 database_name='abt_calibration', dump_path='~/data/dump',
                                 restore_from_dump=config.RestoreMode.NEVER, dump_on_exit=False)

# Collection template seetings
c_str_01 = config.CollectionTemplate(
    name="c_str_01", fields=[
        config.FieldTemplate(name="choice1", data_type=config.DataType.STRING,
                             distribution=distributions['string_choice'], indexed=True)
    ], compound_indexes=[])

c_str_05 = config.CollectionTemplate(
    name="c_str_05", fields=[
        config.FieldTemplate(name="choice1", data_type=config.DataType.STRING,
                             distribution=distributions["string_choice"], indexed=True),
        config.FieldTemplate(name="mixed1", data_type=config.DataType.STRING,
                             distribution=distributions["string_mixed"], indexed=True),
        config.FieldTemplate(name="uniform1", data_type=config.DataType.STRING,
                             distribution=distributions["string_uniform"], indexed=True),
        config.FieldTemplate(name="choice2", data_type=config.DataType.STRING,
                             distribution=distributions["string_choice"], indexed=True),
        config.FieldTemplate(name="mixed2", data_type=config.DataType.STRING,
                             distribution=distributions["string_mixed"], indexed=True),
    ], compound_indexes=[["choice1", "mixed1"]])

# Data Generator settings
data_generator = config.DataGeneratorConfig(enabled=True, collection_cardinalities=[100, 200, 500],
                                            batch_size=10000,
                                            collection_templates=[c_str_01, c_str_05])

# Workload Execution settings
workload_execution = config.WorkloadExecutionConfig(
    enabled=True, output_collection_name='calibrationData', write_mode=config.WriteMode.REPLACE,
    warmup_runs=1, runs=5)

# Calibrator settings
abt_calibrator = config.AbtCalibratorConfig(
    enabled=True, test_size=0.2, input_collection_name=workload_execution.output_collection_name,
    trace=False)

main_config = config.Config(database=database, data_generator=data_generator,
                            abt_calibrator=abt_calibrator, workload_execution=workload_execution)
