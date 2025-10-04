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

"""
Example collection spec to demonstrate various features of this data generator.

Models a mixed spread of documents used to test CE calibrations.

TODO(SERVER-106819): Make this example work once functions from CE calibrations have been integrated
into the data generator.
"""

import random

import datagen.config
import datagen.random
import numpy as np


def generate_calibrations(seed) -> datagen.config.DataGeneratorConfig:
    datagen.random._rng = np.random.default_rng(int(seed))

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

    distributions["string_choice"] = datagen.random.RandomDistribution.choice(
        string_choice_values, string_choice_weights
    )

    small_query_weights = [i for i in range(10, 201, 10)]

    int_choice_values = [i for i in range(1, 1000, 50)]
    random.shuffle(int_choice_values)
    distributions["int_choice"] = datagen.random.RandomDistribution.choice(
        int_choice_values, small_query_weights
    )

    distributions["random_string"] = datagen.random.ArrayRandomDistribution(
        datagen.random.RandomDistribution.uniform(
            datagen.random.RangeGenerator(datagen.random.DataType.INTEGER, 5, 10, 2)
        ),
        datagen.random.RandomDistribution.uniform(
            datagen.random.RangeGenerator(datagen.random.DataType.STRING, "a", "z")
        ),
    )

    def generate_random_str(num: int):
        strs = distributions["random_string"].generate(num)
        str_list = []
        for char_array in strs:
            str_res = "".join(char_array)
            str_list.append(str_res)

        return str_list

    def random_strings_distr(size: int, count: int):
        distr = datagen.random.ArrayRandomDistribution(
            datagen.random.RandomDistribution.uniform([size]),
            datagen.random.RandomDistribution.uniform(
                datagen.random.RangeGenerator(datagen.random.DataType.STRING, "a", "z")
            ),
        )

        return datagen.random.RandomDistribution.uniform(
            ["".join(s) for s in distr.generate(count)]
        )

    small_string_choice = generate_random_str(20)

    distributions["string_choice_small"] = datagen.random.RandomDistribution.choice(
        small_string_choice, small_query_weights
    )

    string_range_4 = datagen.random.RandomDistribution.normal(
        datagen.random.RangeGenerator(datagen.random.DataType.STRING, "abca", "abc_")
    )
    string_range_5 = datagen.random.RandomDistribution.normal(
        datagen.random.RangeGenerator(datagen.random.DataType.STRING, "abcda", "abcd_")
    )
    string_range_7 = datagen.random.RandomDistribution.normal(
        datagen.random.RangeGenerator(datagen.random.DataType.STRING, "hello_a", "hello__")
    )
    string_range_12 = datagen.random.RandomDistribution.normal(
        datagen.random.RangeGenerator(
            datagen.random.DataType.STRING, "helloworldaa", "helloworldd_"
        )
    )

    distributions["string_mixed"] = datagen.random.RandomDistribution.mixed(
        [string_range_4, string_range_5, string_range_7, string_range_12], [0.1, 0.15, 0.25, 0.5]
    )

    distributions["string_uniform"] = datagen.random.RandomDistribution.uniform(
        datagen.random.RangeGenerator(
            datagen.random.DataType.STRING, "helloworldaa", "helloworldd_"
        )
    )

    distributions["int_normal"] = datagen.random.RandomDistribution.normal(
        datagen.random.RangeGenerator(datagen.random.DataType.INTEGER, 0, 1000, 2)
    )

    lengths_distr = datagen.random.RandomDistribution.uniform(
        datagen.random.RangeGenerator(datagen.random.DataType.INTEGER, 1, 10)
    )
    distributions["array_small"] = datagen.random.ArrayRandomDistribution(
        lengths_distr, distributions["int_normal"]
    )

    collection_cardinalities = list(range(10000, 50001, 10000))

    c_int_05 = datagen.config.CollectionTemplate(
        name="c_int_05",
        fields=[
            datagen.config.FieldTemplate(
                name="in1",
                data_type=datagen.config.datagen.random.DataType.INTEGER,
                distribution=distributions["int_normal"],
                indexed=True,
            ),
            datagen.config.FieldTemplate(
                name="mixed1",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="uniform1",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_uniform"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="in2",
                data_type=datagen.config.datagen.random.DataType.INTEGER,
                distribution=distributions["int_normal"],
                indexed=True,
            ),
            datagen.config.FieldTemplate(
                name="mixed2",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
        ],
        compound_indexes=[],
        cardinalities=collection_cardinalities,
    )

    c_arr_01 = datagen.config.CollectionTemplate(
        name="c_arr_01",
        fields=[
            datagen.config.FieldTemplate(
                name="as",
                data_type=datagen.config.datagen.random.DataType.INTEGER,
                distribution=distributions["array_small"],
                indexed=True,
            )
        ],
        compound_indexes=[],
        cardinalities=collection_cardinalities,
    )

    # A string value to fill up collections and not used in queries.
    HIDDEN_STRING_VALUE = "__hidden_string_value"

    index_scan = datagen.config.CollectionTemplate(
        name="index_scan",
        fields=[
            datagen.config.FieldTemplate(
                name="choice",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=datagen.random.RandomDistribution.choice(
                    [
                        "iqtbr5b5is",
                        "vt5s3tf8o6",
                        "b0rgm58qsn",
                        "9m59if353m",
                        "biw2l9ok17",
                        "b9ct0ue14d",
                        "oxj0vxjsti",
                        "f3k8w9vb49",
                        "ec7v82k6nk",
                        "f49ufwaqx7",
                        HIDDEN_STRING_VALUE,
                    ],
                    [
                        *range(10, 260, 25),
                        1000000 - sum(range(10, 260, 25)),
                    ],
                ),
                indexed=True,
            ),
            datagen.config.FieldTemplate(
                name="mixed1",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="uniform1",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_uniform"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="choice2",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_choice"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="mixed2",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
        ],
        compound_indexes=[],
        cardinalities=[1000000],
    )

    physical_scan = datagen.config.CollectionTemplate(
        name="physical_scan",
        fields=[
            datagen.config.FieldTemplate(
                name="choice1",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_choice"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="mixed1",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="uniform1",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_uniform"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="choice",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_choice"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="mixed2",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=distributions["string_mixed"],
                indexed=False,
            ),
            datagen.config.FieldTemplate(
                name="payload",
                data_type=datagen.config.datagen.random.DataType.STRING,
                distribution=random_strings_distr(2000, 1000),
                indexed=False,
            ),
        ],
        compound_indexes=[],
        cardinalities=[1000, 5000, 10000],
    )

    # Data Generator settings
    return datagen.config.DataGeneratorConfig(
        enabled=True,
        create_indexes=True,
        batch_size=10000,
        collection_templates=[index_scan, physical_scan, c_int_05, c_arr_01],
        write_mode=datagen.config.WriteMode.REPLACE,
        collection_name_with_card=True,
    )
