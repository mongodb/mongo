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

import random
import config
from random_generator import RangeGenerator, DataType, RandomDistribution, ArrayRandomDistribution

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

small_query_weights = [i for i in range(10, 201, 10)]
small_query_cardinality = sum(small_query_weights)

int_choice_values = [i for i in range(1, 1000, 50)]
random.shuffle(int_choice_values)
distributions['int_choice'] = RandomDistribution.choice(int_choice_values, small_query_weights)

distributions['random_string'] = ArrayRandomDistribution(
    RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 5, 10, 2)),
    RandomDistribution.uniform(RangeGenerator(DataType.STRING, "a", "z")))


def generate_random_str(num: int):
    strs = distributions['random_string'].generate(num)
    str_list = []
    for char_array in strs:
        str_res = "".join(char_array)
        str_list.append(str_res)

    return str_list


small_string_choice = generate_random_str(20)

distributions['string_choice_small'] = RandomDistribution.choice(small_string_choice,
                                                                 small_query_weights)

string_range_4 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "abca", "abc_"))
string_range_5 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "abcda", "abcd_"))
string_range_7 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "hello_a", "hello__"))
string_range_12 = RandomDistribution.normal(
    RangeGenerator(DataType.STRING, "helloworldaa", "helloworldd_"))

distributions['string_mixed'] = RandomDistribution.mixed(
    [string_range_4, string_range_5, string_range_7, string_range_12], [0.1, 0.15, 0.25, 0.5])

distributions['string_uniform'] = RandomDistribution.uniform(
    RangeGenerator(DataType.STRING, "helloworldaa", "helloworldd_"))

distributions['int_normal'] = RandomDistribution.normal(
    RangeGenerator(DataType.INTEGER, 0, 1000, 2))

lengths_distr = RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 10))
distributions['array_small'] = ArrayRandomDistribution(lengths_distr, distributions['int_normal'])

# Database settings
database = config.DatabaseConfig(connection_string='mongodb://localhost',
                                 database_name='abt_calibration_small', dump_path='~/data/dump',
                                 restore_from_dump=config.RestoreMode.NEVER, dump_on_exit=False)

# Collection template seetings

# templates for small queries.
c_int_05_small = config.CollectionTemplate(
    name="c_int_05", fields=[
        config.FieldTemplate(name="in1", data_type=config.DataType.INTEGER,
                             distribution=distributions["int_choice"], indexed=True),
        config.FieldTemplate(name="mixed1", data_type=config.DataType.STRING,
                             distribution=distributions["string_mixed"], indexed=False),
        config.FieldTemplate(name="uniform1", data_type=config.DataType.STRING,
                             distribution=distributions["string_uniform"], indexed=False),
        config.FieldTemplate(name="in2", data_type=config.DataType.INTEGER,
                             distribution=distributions["int_choice"], indexed=True),
        config.FieldTemplate(name="mixed2", data_type=config.DataType.STRING,
                             distribution=distributions["string_mixed"], indexed=False),
    ], compound_indexes=[])

c_str_02_small = config.CollectionTemplate(
    name="c_str_02", fields=[
        config.FieldTemplate(name="choice1", data_type=config.DataType.STRING,
                             distribution=distributions['string_choice_small'], indexed=True),
        config.FieldTemplate(name="choice2", data_type=config.DataType.STRING,
                             distribution=distributions['string_choice_small'], indexed=False)
    ], compound_indexes=[])

c_arr_01_small = config.CollectionTemplate(
    name="c_arr_01", fields=[
        config.FieldTemplate(name="as", data_type=config.DataType.INTEGER,
                             distribution=distributions["array_small"], indexed=False),
        config.FieldTemplate(name="in1", data_type=config.DataType.INTEGER,
                             distribution=distributions["int_choice"], indexed=True),
    ], compound_indexes=[])

c_str_01 = config.CollectionTemplate(
    name="c_str_01", fields=[
        config.FieldTemplate(name="choice1", data_type=config.DataType.STRING,
                             distribution=distributions['string_choice'], indexed=True)
    ], compound_indexes=[])

c_str_02 = config.CollectionTemplate(
    name="c_str_02", fields=[
        config.FieldTemplate(name="choice1", data_type=config.DataType.STRING,
                             distribution=distributions['string_choice'], indexed=True),
        config.FieldTemplate(name="choice2", data_type=config.DataType.STRING,
                             distribution=distributions['string_choice'], indexed=False)
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

c_int_05 = config.CollectionTemplate(
    name="c_int_05", fields=[
        config.FieldTemplate(name="in1", data_type=config.DataType.INTEGER,
                             distribution=distributions["int_normal"], indexed=True),
        config.FieldTemplate(name="mixed1", data_type=config.DataType.STRING,
                             distribution=distributions["string_mixed"], indexed=False),
        config.FieldTemplate(name="uniform1", data_type=config.DataType.STRING,
                             distribution=distributions["string_uniform"], indexed=False),
        config.FieldTemplate(name="in2", data_type=config.DataType.INTEGER,
                             distribution=distributions["int_normal"], indexed=True),
        config.FieldTemplate(name="mixed2", data_type=config.DataType.STRING,
                             distribution=distributions["string_mixed"], indexed=False),
    ], compound_indexes=[])

c_arr_01 = config.CollectionTemplate(
    name="c_arr_01", fields=[
        config.FieldTemplate(name="as", data_type=config.DataType.INTEGER,
                             distribution=distributions["array_small"], indexed=False)
    ], compound_indexes=[])

# Data Generator settings
data_generator = config.DataGeneratorConfig(
    enabled=True, collection_cardinalities=list(range(10000, 50001,
                                                      2500)), collection_name_with_card=True,
    batch_size=10000, collection_templates=[c_str_01, c_str_05, c_int_05,
                                            c_arr_01], write_mode=config.WriteMode.REPLACE)
"""Data generation settings for Calibration with queries that return smaller number of documents.
# First round of data population generates a small set of documents that will be queried.

data_generator = config.DataGeneratorConfig(
    enabled=True, collection_cardinalities=[small_query_cardinality], collection_name_with_card=False, batch_size=10000,
    collection_templates=[c_str_02_small, c_int_05_small, c_arr_01_small], write_mode=config.WriteMode.APPEND)

# Second round of data population which generates 100,000 documents to the collection in order to
# make sure the collection is big enough for queries.

data_generator = config.DataGeneratorConfig(
    enabled=True, collection_cardinalities=[100000], collection_name_with_card=False, batch_size=10000,
    collection_templates=[c_str_02, c_int_05, c_arr_01], write_mode=config.WriteMode.APPEND)

# Please note that the 'WriteMode' should be 'APPEND' type and the collection name should remain
# unchanged. This can be controlled by setting "collection_name_with_card=False".
"""

# Workload Execution settings
workload_execution = config.WorkloadExecutionConfig(
    enabled=True, output_collection_name='calibrationDataSmall', write_mode=config.WriteMode.APPEND,
    warmup_runs=3, runs=10)

abt_nodes = [
    config.AbtNodeCalibrationConfig(type='PhysicalScan'),
    config.AbtNodeCalibrationConfig(type='IndexScan'),
    config.AbtNodeCalibrationConfig(type='Seek'),
    config.AbtNodeCalibrationConfig(type='Filter'),
    config.AbtNodeCalibrationConfig(type='Evaluation'),
    config.AbtNodeCalibrationConfig(type='BinaryJoin'),
    config.AbtNodeCalibrationConfig(type='HashJoin'),
    config.AbtNodeCalibrationConfig(type='MergeJoin'),
    config.AbtNodeCalibrationConfig(type='Union'),
    config.AbtNodeCalibrationConfig(type='LimitSkip'),
    config.AbtNodeCalibrationConfig(type='GroupBy'),
    config.AbtNodeCalibrationConfig(type='Unwind'),
]
# Calibrator settings
abt_calibrator = config.AbtCalibratorConfig(
    enabled=True, test_size=0.2, input_collection_name=workload_execution.output_collection_name,
    trace=False, nodes=abt_nodes)

main_config = config.Config(database=database, data_generator=data_generator,
                            abt_calibrator=abt_calibrator, workload_execution=workload_execution)
