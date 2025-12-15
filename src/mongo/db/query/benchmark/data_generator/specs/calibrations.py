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
"""

import dataclasses
import random

from datagen.distribution import *
from datagen.util import Specification
from datagen.values import RangeGenerator
from pymongo import IndexModel

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

distributions["string_choice"] = choice(string_choice_values, string_choice_weights)

small_query_weights = [i for i in range(10, 201, 10)]

int_choice_values = [i for i in range(1, 1000, 50)]
random.shuffle(int_choice_values)
distributions["int_choice"] = choice(int_choice_values, small_query_weights)

distributions["random_string"] = array(
    uniform(RangeGenerator(int, 5, 10, 2)),
    uniform(RangeGenerator(str, "a", "z")),
)


def generate_random_str(num: int):
    strs = [distributions["random_string"]() for _ in range(num)]

    def result_func():
        str_list = []
        for char_array in strs:
            str_res = "".join(char_array)
            str_list.append(str_res)

        return str_list

    return result_func


def random_strings_distr(size: int, count: int):
    distr = array(
        size,
        uniform(RangeGenerator(str, "a", "z")),
    )

    return uniform(["".join(distr()) for _ in range(count)])


small_string_choice = generate_random_str(20)

distributions["string_choice_small"] = choice(small_string_choice, small_query_weights)

string_range_4 = normal(RangeGenerator(str, "abca", "abc_"))
string_range_5 = normal(RangeGenerator(str, "abcda", "abcd_"))
string_range_7 = normal(RangeGenerator(str, "hello_a", "hello__"))
string_range_12 = normal(RangeGenerator(str, "helloworldaa", "helloworldd_"))

distributions["string_mixed"] = choice(
    [string_range_4, string_range_5, string_range_7, string_range_12], [0.1, 0.15, 0.25, 0.5]
)

distributions["string_uniform"] = uniform(RangeGenerator(str, "helloworldaa", "helloworldd_"))

distributions["int_normal"] = normal(RangeGenerator(int, 0, 1000, 2))

lengths_distr = uniform(RangeGenerator(int, 1, 10))
distributions["array_small"] = array(lengths_distr, distributions["int_normal"])


# # Recommended sizes: 10000, 20000, 30000, 40000, 50000
@dataclasses.dataclass
class c_int_05:
    # index this
    in1: Specification(distributions["int_normal"])
    mixed1: Specification(distributions["string_mixed"])
    uniform1: Specification(distributions["string_uniform"])
    # index this
    in2: Specification(distributions["int_normal"])
    mixed2: Specification(distributions["string_mixed"])


c_int_05_idx = [IndexModel(keys="in1"), IndexModel(keys="in2")]


# Recommended sizes: 10000, 20000, 30000, 40000, 50000
@dataclasses.dataclass
class c_arr_01:
    # This is intended to be indexed
    as_: Specification(distributions["array_small"])


c_arr_01_idx = [IndexModel(keys="as_")]


# # Recommended size: 1000000
@dataclasses.dataclass
class index_scan:
    choice1: Specification(
        choice(
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
                "__hidden_string_value",
            ],
            [
                *range(10, 260, 25),
                1000000 - sum(range(10, 260, 25)),
            ],
        )
    )
    mixed1: Specification(distributions["string_mixed"])
    uniform1: Specification(distributions["string_uniform"])
    choice2: Specification(distributions["string_choice"])
    mixed2: Specification(distributions["string_mixed"])


index_scan_idx = [IndexModel(keys="choice1")]


# # It is recommended to create this collection with sizes of 1000, 5000, 10000
@dataclasses.dataclass
class physical_scan:
    choice1: Specification(distributions["string_choice"])
    mixed1: Specification(distributions["string_mixed"])
    uniform1: Specification(distributions["string_uniform"])
    choice: Specification(distributions["string_choice"])
    mixed2: Specification(distributions["string_mixed"])
    payload: Specification(random_strings_distr(2000, 1000))
