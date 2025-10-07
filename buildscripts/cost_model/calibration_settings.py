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

import os
import random

import config
import numpy as np
import pandas as pd
from random_generator import ArrayRandomDistribution, DataType, RandomDistribution, RangeGenerator

__all__ = ["main_config", "distributions"]

# A string value to fill up collections and not used in queries.
HIDDEN_STRING_VALUE = "__hidden_string_value"

# Data distributions settings.
distributions = {}

string_choice_values = [
    "h",
    "hi",
    "hi!",
    "hola",
    "hello",
    "square",
    "squared",
    "gaussian",
    "chisquare",
    "chisquared",
    "hello world",
    "distribution",
]

string_choice_weights = [10, 20, 5, 17, 30, 7, 9, 15, 40, 2, 12, 1]

distributions["string_choice"] = RandomDistribution.choice(
    string_choice_values, string_choice_weights
)

small_query_weights = [i for i in range(10, 201, 10)]
small_query_cardinality = sum(small_query_weights)

int_choice_values = [i for i in range(1, 1000, 50)]
random.shuffle(int_choice_values)
distributions["int_choice"] = RandomDistribution.choice(int_choice_values, small_query_weights)

distributions["random_string"] = ArrayRandomDistribution(
    RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 5, 10, 2)),
    RandomDistribution.uniform(RangeGenerator(DataType.STRING, "a", "z")),
)


def generate_random_str(num: int):
    strs = distributions["random_string"].generate(num)
    str_list = []
    for char_array in strs:
        str_res = "".join(char_array)
        str_list.append(str_res)

    return str_list


def random_strings_distr(size: int, count: int):
    distr = ArrayRandomDistribution(
        RandomDistribution.uniform([size]),
        RandomDistribution.uniform(RangeGenerator(DataType.STRING, "a", "z")),
    )

    return RandomDistribution.uniform(["".join(s) for s in distr.generate(count)])


small_string_choice = generate_random_str(20)

distributions["string_choice_small"] = RandomDistribution.choice(
    small_string_choice, small_query_weights
)

string_range_4 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "abca", "abc_"))
string_range_5 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "abcda", "abcd_"))
string_range_7 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "hello_a", "hello__"))
string_range_12 = RandomDistribution.normal(
    RangeGenerator(DataType.STRING, "helloworldaa", "helloworldd_")
)

distributions["string_mixed"] = RandomDistribution.mixed(
    [string_range_4, string_range_5, string_range_7, string_range_12], [0.1, 0.15, 0.25, 0.5]
)

distributions["string_uniform"] = RandomDistribution.uniform(
    RangeGenerator(DataType.STRING, "helloworldaa", "helloworldd_")
)

distributions["int_normal"] = RandomDistribution.normal(
    RangeGenerator(DataType.INTEGER, 0, 1000, 2)
)

lengths_distr = RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 10))
distributions["array_small"] = ArrayRandomDistribution(lengths_distr, distributions["int_normal"])

# Database settings
database = config.DatabaseConfig(
    connection_string=os.getenv("MONGODB_URI", "mongodb://localhost"),
    database_name="qsn_calibration",
    dump_path="~/mongo/buildscripts/cost_model",
    restore_from_dump=config.RestoreMode.NEVER,
    dump_on_exit=False,
)


# Collection template settings
def create_coll_scan_collection_template(
    name: str, cardinalities: list[int], payload_size: int = 0
) -> config.CollectionTemplate:
    template = config.CollectionTemplate(
        name=name,
        fields=[
            config.FieldTemplate(
                name="choice1",
                data_type=config.DataType.STRING,
                distribution=distributions["string_choice"],
                indexed=False,
            ),
            config.FieldTemplate(
                name="mixed1",
                data_type=config.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
            config.FieldTemplate(
                name="uniform1",
                data_type=config.DataType.STRING,
                distribution=distributions["string_uniform"],
                indexed=False,
            ),
            config.FieldTemplate(
                name="choice",
                data_type=config.DataType.STRING,
                distribution=distributions["string_choice"],
                indexed=False,
            ),
            config.FieldTemplate(
                name="mixed2",
                data_type=config.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
            config.FieldTemplate(
                name="int_uniform",
                data_type=config.DataType.INTEGER,
                distribution=RandomDistribution.uniform(
                    RangeGenerator(DataType.INTEGER, 0, 100_000)
                ),
                indexed=True,
            ),
            config.FieldTemplate(
                name="int_uniform_unindexed",
                data_type=config.DataType.INTEGER,
                distribution=RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 2)),
                indexed=False,
            ),
        ],
        compound_indexes=[],
        cardinalities=cardinalities,
    )

    if payload_size > 0:
        payload_distr = random_strings_distr(payload_size, 1000)
        template.fields.append(
            config.FieldTemplate(
                name="payload",
                data_type=config.DataType.STRING,
                distribution=payload_distr,
                indexed=False,
            )
        )
    return template


def create_intersection_collection_template(
    name: str, cardinalities: list[int], distribution: str, value_range: int = 10
) -> config.CollectionTemplate:
    distribution_fn = (
        RandomDistribution.normal if distribution == "normal" else RandomDistribution.uniform
    )

    fields = [
        config.FieldTemplate(
            name="a",
            data_type=config.DataType.INTEGER,
            distribution=distribution_fn(RangeGenerator(DataType.INTEGER, 1, value_range + 1)),
            indexed=True,
        ),
        config.FieldTemplate(
            name="b",
            data_type=config.DataType.INTEGER,
            distribution=distribution_fn(RangeGenerator(DataType.INTEGER, 1, value_range + 1)),
            indexed=True,
        ),
    ]

    return config.CollectionTemplate(
        name=name,
        fields=fields,
        compound_indexes=[],
        cardinalities=cardinalities,
    )


# Creates a collection with fields "a", "b", ... "j" (if 'num_fields' is 10) and an
# additional field "sort_field" if 'include_sort_field' is true.
# If 'every_field_indexed' is false then only "a" will be indexed.
# 'end_of_range_is_card' requires that there is only one cardinality in
# 'cardinalities' and sets the end of the range for the field values to be the cardinality.
def create_indexed_fields_template(
    name: str,
    cardinalities: list[int],
    end_of_range_is_card,
    every_field_indexed,
    include_sort_field,
    num_base_fields: int = 10,
) -> config.CollectionTemplate:
    # Generate fields "a", "b", ... "j" (if num_merge_fields is 10)
    field_names = [chr(ord("a") + i) for i in range(num_base_fields)]

    dist_end_range = num_base_fields + 1
    if end_of_range_is_card:
        assert len(cardinalities) == 1
        dist_end_range = cardinalities[0]

    fields = [
        config.FieldTemplate(
            name=field_name,
            data_type=config.DataType.INTEGER,
            distribution=RandomDistribution.uniform(
                RangeGenerator(DataType.INTEGER, 1, dist_end_range)
            ),
            indexed=True if every_field_indexed else (field_name == "a"),
        )
        for field_name in field_names
    ]

    compound_indexes = []

    if include_sort_field:
        fields.append(
            config.FieldTemplate(
                name="sort_field",
                data_type=config.DataType.STRING,
                distribution=random_strings_distr(10, 1000),
                indexed=False,
            )
        )
        compound_indexes = [{field_name: 1, "sort_field": 1} for field_name in field_names]

    elif not every_field_indexed:
        assert num_base_fields == 10
        compound_indexes = [
            # Note the single field index is created in the FieldTemplate for 'a' above.
            ["a", "b"],
            ["a", "b", "c"],
            ["a", "b", "c", "d"],
            ["a", "b", "c", "d", "e"],
            ["a", "b", "c", "d", "e", "f"],
            ["a", "b", "c", "d", "e", "f", "g"],
            ["a", "b", "c", "d", "e", "f", "g", "h"],
            ["a", "b", "c", "d", "e", "f", "g", "h", "i"],
            ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"],
        ]

    return config.CollectionTemplate(
        name=name,
        fields=fields,
        compound_indexes=compound_indexes,
        cardinalities=cardinalities,
    )


projection_collection = config.CollectionTemplate(
    name="projection",
    fields=[
        config.FieldTemplate(
            name="in1",
            data_type=config.DataType.INTEGER,
            distribution=distributions["int_normal"],
            indexed=True,
        ),
        config.FieldTemplate(
            name="mixed1",
            data_type=config.DataType.STRING,
            distribution=distributions["string_mixed"],
            indexed=False,
        ),
        config.FieldTemplate(
            name="uniform1",
            data_type=config.DataType.STRING,
            distribution=distributions["string_uniform"],
            indexed=False,
        ),
        config.FieldTemplate(
            name="in2",
            data_type=config.DataType.INTEGER,
            distribution=distributions["int_normal"],
            indexed=True,
        ),
        config.FieldTemplate(
            name="mixed2",
            data_type=config.DataType.STRING,
            distribution=distributions["string_mixed"],
            indexed=False,
        ),
    ],
    compound_indexes=[],
    cardinalities=[30000],
)

doc_scan_collection = create_coll_scan_collection_template(
    "doc_scan", cardinalities=[100_000], payload_size=2000
)
sort_collections = create_coll_scan_collection_template(
    "sort",
    cardinalities=[5, 10, 50, 75, 100, 150, 300, 400, 500, 750, 1000],
    payload_size=10,
)
merge_sort_collections = create_indexed_fields_template(
    "merge_sort",
    cardinalities=[5, 10, 50, 75, 100, 150, 300, 400, 500, 750, 1000],
    end_of_range_is_card=False,
    every_field_indexed=False,
    include_sort_field=True,
    num_base_fields=10,
)
or_collections = create_indexed_fields_template(
    "or",
    cardinalities=[5, 10, 50, 75, 100, 150, 300, 400, 500, 750] + list(range(1000, 10001, 1000)),
    end_of_range_is_card=False,
    every_field_indexed=True,
    include_sort_field=False,
    num_base_fields=2,
)
intersection_sorted_collections = create_intersection_collection_template(
    "intersection_sorted",
    distribution="normal",
    cardinalities=[5, 100, 1000, 5000],
    value_range=10,
)
intersection_hash_collection = create_intersection_collection_template(
    "intersection_hash",
    distribution="normal",
    cardinalities=[1000],
    value_range=10,
)

index_scan_collection = create_indexed_fields_template(
    "index_scan",
    cardinalities=[10000],
    end_of_range_is_card=True,
    every_field_indexed=False,
    include_sort_field=False,
    num_base_fields=10,
)

# Data Generator settings
data_generator = config.DataGeneratorConfig(
    enabled=True,
    create_indexes=True,
    batch_size=10000,
    collection_templates=[
        index_scan_collection,
        doc_scan_collection,
        sort_collections,
        merge_sort_collections,
        or_collections,
        intersection_sorted_collections,
        intersection_hash_collection,
        projection_collection,
    ],
    write_mode=config.WriteMode.REPLACE,
    collection_name_with_card=True,
)

# Workload Execution settings
workload_execution = config.WorkloadExecutionConfig(
    enabled=True,
    output_collection_name="calibrationData",
    write_mode=config.WriteMode.REPLACE,
    warmup_runs=10,
    runs=100,
)

qsn_nodes = [
    config.QsNodeCalibrationConfig(name="COLLSCAN_FORWARD", type="COLLSCAN"),
    config.QsNodeCalibrationConfig(name="COLLSCAN_BACKWARD", type="COLLSCAN"),
    config.QsNodeCalibrationConfig(name="COLLSCAN_W_FILTER", type="COLLSCAN"),
    config.QsNodeCalibrationConfig(
        name="IXSCAN_FORWARD",
        type="IXSCAN",
        variables_override=lambda df: pd.concat(
            [df["n_processed"].rename("Keys Examined"), df["seeks"].rename("Number of seeks")],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        name="IXSCAN_BACKWARD",
        type="IXSCAN",
        variables_override=lambda df: pd.concat(
            [df["n_processed"].rename("Keys Examined"), df["seeks"].rename("Number of seeks")],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        name="IXSCANS_W_DIFF_NUM_FIELDS",
        type="IXSCAN",
        variables_override=lambda df: pd.concat(
            [df["n_index_fields"].rename("Number of fields in index")],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        name="IXSCAN_W_FILTER",
        type="IXSCAN",
        variables_override=lambda df: pd.concat(
            [df["n_processed"].rename("Keys Examined"), df["seeks"].rename("Number of seeks")],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(type="FETCH"),
    config.QsNodeCalibrationConfig(name="FETCH_W_FILTER", type="FETCH"),
    config.QsNodeCalibrationConfig(
        type="AND_HASH",
        variables_override=lambda df: pd.concat(
            [
                df["n_processed_per_child"].str[0].rename("Documents from first child"),
                df["n_processed_per_child"].str[1].rename("Documents from second child"),
                df["n_returned"],
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        type="AND_SORTED",
        variables_override=lambda df: pd.concat(
            [
                df["n_processed"],
                df["n_returned"],
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(type="OR"),
    config.QsNodeCalibrationConfig(
        type="SORT_MERGE",
        # Note: n_returned = n_processed - (amount of duplicates dropped)
        variables_override=lambda df: pd.concat(
            [
                (df["n_returned"] * np.log2(df["n_children"])).rename(
                    "n_returned * log2(n_children)"
                ),
                df["n_processed"],
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        name="SORT_DEFAULT",
        type="SORT",
        # Calibration involves a combination of a linearithmic and linear factor
        variables_override=lambda df: pd.concat(
            [
                (df["n_processed"] * np.log2(df["n_processed"])).rename(
                    "n_processed * log2(n_processed)"
                ),
                df["n_processed"],
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        name="SORT_SIMPLE",
        type="SORT",
        # Calibration involves a combination of a linearithmic and linear factor
        variables_override=lambda df: pd.concat(
            [
                (df["n_processed"] * np.log2(df["n_processed"])).rename(
                    "n_processed * log2(n_processed)"
                ),
                df["n_processed"],
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        name="SORT_LIMIT_SIMPLE",
        type="SORT",
        # Note: n_returned = min(limitAmount, n_processed)
        variables_override=lambda df: pd.concat(
            [
                df["n_processed"],
                (df["n_processed"] * np.log2(df["n_returned"])).rename(
                    "n_processed * log2(n_returned)"
                ),
                (df["n_returned"] * np.log2(df["n_returned"])).rename(
                    "n_returned * log2(n_returned)"
                ),
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(
        name="SORT_LIMIT_DEFAULT",
        type="SORT",
        # Note: n_returned = min(limitAmount, n_processed)
        variables_override=lambda df: pd.concat(
            [
                df["n_processed"],
                (df["n_processed"] * np.log2(df["n_returned"])).rename(
                    "n_processed * log2(n_returned)"
                ),
                (df["n_returned"] * np.log2(df["n_returned"])).rename(
                    "n_returned * log2(n_returned)"
                ),
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(type="LIMIT"),
    config.QsNodeCalibrationConfig(
        type="SKIP",
        variables_override=lambda df: pd.concat(
            [
                df["n_returned"].rename("Documents Passed"),
                (df["n_processed"] - df["n_returned"]).rename("Documents Skipped"),
            ],
            axis=1,
        ),
    ),
    config.QsNodeCalibrationConfig(type="PROJECTION_SIMPLE"),
    config.QsNodeCalibrationConfig(type="PROJECTION_COVERED"),
    config.QsNodeCalibrationConfig(type="PROJECTION_DEFAULT"),
]
# Calibrator settings
qs_calibrator = config.QuerySolutionCalibrationConfig(
    enabled=True,
    test_size=0.2,
    input_collection_name=workload_execution.output_collection_name,
    trace=False,
    nodes=qsn_nodes,
)


main_config = config.Config(
    database=database,
    data_generator=data_generator,
    qs_calibrator=qs_calibrator,
    workload_execution=workload_execution,
)
