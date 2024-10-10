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
"""Configuration of data generation for CE accuracy testing."""

from datetime import datetime
from pathlib import Path
from typing import Sequence

import config
from random_generator import (
    ArrayRandomDistribution,
    DataType,
    DistributionType,
    RandomDistribution,
    RangeGenerator,
)

__all__ = ["database_config", "data_generator_config"]

################################################################################
# Data distributions
################################################################################


def add_distribution(
    distr_set: Sequence[RandomDistribution], distr_type: DistributionType, rg: RangeGenerator
):
    distr = None
    if distr_type == DistributionType.UNIFORM:
        distr = RandomDistribution.uniform(rg)
    elif distr_type == DistributionType.NORMAL:
        distr = RandomDistribution.normal(rg)
    elif distr_type == DistributionType.CHI2:
        distr = RandomDistribution.noncentral_chisquare(rg)
    else:
        raise ValueError("Unknown distribution")
    distr_set.append(distr)


# Ranges
int_ranges_1 = [
    # 1K unique integers with different distances
    RangeGenerator(DataType.INTEGER, 0, 1000, 1),
    RangeGenerator(DataType.INTEGER, 0, 10000, 10),
    RangeGenerator(DataType.INTEGER, 0, 100000, 100),
    # 10K unique integers with different distances
    RangeGenerator(DataType.INTEGER, 0, 10000, 1),
    RangeGenerator(DataType.INTEGER, 0, 1000000, 10),
    RangeGenerator(DataType.INTEGER, 0, 10000000, 100),
]

int_ranges_2 = [
    # 1K unique integers with different distances
    RangeGenerator(DataType.INTEGER, 7000, 8000, 1),
    RangeGenerator(DataType.INTEGER, 70000, 80000, 10),
    RangeGenerator(DataType.INTEGER, 700000, 800000, 100),
    # 10K unique integers with different distances
    RangeGenerator(DataType.INTEGER, 70000, 80000, 1),
    RangeGenerator(DataType.INTEGER, 700000, 800000, 10),
    RangeGenerator(DataType.INTEGER, 7000000, 8000000, 100),
]

#######################
# Integer distributions

int_distributions = []

for range_gen in int_ranges_1:
    add_distribution(int_distributions, DistributionType.UNIFORM, range_gen)
    add_distribution(int_distributions, DistributionType.NORMAL, range_gen)
    add_distribution(int_distributions, DistributionType.CHI2, range_gen)

# Distributions to be used only in other mixed distributions
int_distributions_offset = []
for range_gen in int_ranges_2:
    add_distribution(int_distributions_offset, DistributionType.UNIFORM, range_gen)
    add_distribution(int_distributions_offset, DistributionType.NORMAL, range_gen)
    add_distribution(int_distributions_offset, DistributionType.CHI2, range_gen)

# Mixes of distributions with different NDV and value distances
int_distributions.append(
    RandomDistribution.mixed(
        children=[int_distributions[0], int_distributions_offset[0], int_distributions[4]],
        weight=[1, 1, 1],
    )
)

int_distributions.append(
    RandomDistribution.mixed(
        children=[int_distributions[1], int_distributions[4], int_distributions[7]],
        weight=[1, 1, 1],
    )
)

int_distributions.append(
    RandomDistribution.mixed(
        children=[
            int_distributions[1],
            int_distributions_offset[1],
            int_distributions[3],
            int_distributions[2],
            int_distributions_offset[2],
        ],
        weight=[1, 1, 1, 1, 1],
    )
)

int_distributions.append(
    RandomDistribution.mixed(
        children=[
            int_distributions[2],
            int_distributions[3],
            int_distributions[6],
            int_distributions_offset[1],
            int_distributions_offset[2],
            int_distributions_offset[5],
        ],
        weight=[1, 1, 1, 1, 1, 1],
    )
)

#############################
# Double number distributions

dbl_ranges = [
    # 1K unique doubles with different distances
    RangeGenerator(DataType.DOUBLE, 0.0, 100.0, 0.1),
    RangeGenerator(DataType.DOUBLE, 0.0, 10000.0, 10),
    RangeGenerator(DataType.DOUBLE, 0.0, 1000000.0, 1000),
    # 10K unique doubles with different distances
    RangeGenerator(DataType.DOUBLE, 0.0, 1000.0, 0.1),
    RangeGenerator(DataType.DOUBLE, 0.0, 100000.0, 10),
    RangeGenerator(DataType.DOUBLE, 0.0, 10000000.0, 1000),
]

dbl_distributions = []

for range_gen in dbl_ranges:
    add_distribution(dbl_distributions, DistributionType.UNIFORM, range_gen)
    add_distribution(dbl_distributions, DistributionType.NORMAL, range_gen)

dbl_distributions.append(
    RandomDistribution.mixed(
        children=[dbl_distributions[0], dbl_distributions[3], dbl_distributions[10]],
        weight=[1, 1, 1],
    )
)

dbl_distributions.append(
    RandomDistribution.mixed(
        children=[
            dbl_distributions[0],
            dbl_distributions[4],
            RandomDistribution.normal(RangeGenerator(DataType.DOUBLE, 500.0, 600.0, 0.1)),
            RandomDistribution.normal(RangeGenerator(DataType.DOUBLE, 3000200.0, 5000100.0, 3030)),
        ],
        weight=[1, 1, 1, 1],
    )
)

#############################
# Date distributions

MINUTE = 60
HOUR = MINUTE * 60
DAY = HOUR * 24
MONTH = DAY * 30

range_dtt_1y = RangeGenerator(DataType.DATE, datetime(2007, 1, 1), datetime(2008, 1, 1), HOUR)
range_dtt_1m_1 = RangeGenerator(DataType.DATE, datetime(2007, 2, 1), datetime(2008, 3, 1), HOUR)
range_dtt_1m_2 = RangeGenerator(DataType.DATE, datetime(2007, 6, 1), datetime(2008, 7, 1), HOUR)
range_dtt_1m_3 = RangeGenerator(DataType.DATE, datetime(2007, 10, 1), datetime(2008, 11, 1), HOUR)
range_dtt_10y_1 = RangeGenerator(DataType.DATE, datetime(2006, 1, 1), datetime(2016, 1, 1), DAY)
range_dtt_10y_2 = RangeGenerator(DataType.DATE, datetime(1995, 1, 1), datetime(2005, 1, 1), DAY)
range_dtt_20y = RangeGenerator(DataType.DATE, datetime(1997, 10, 1), datetime(2017, 11, 1), MONTH)

dt_distributions = []

add_distribution(dt_distributions, DistributionType.UNIFORM, range_dtt_1y)
add_distribution(dt_distributions, DistributionType.NORMAL, range_dtt_10y_1)

dt_distributions.append(
    RandomDistribution.mixed(
        [
            RandomDistribution.uniform(range_dtt_1y),
            RandomDistribution.uniform(range_dtt_1m_1),
            RandomDistribution.uniform(range_dtt_1m_2),
            RandomDistribution.uniform(range_dtt_1m_3),
        ],
        [1, 1, 1, 1],
    )
)

dt_distributions.append(
    RandomDistribution.mixed(
        [
            RandomDistribution.uniform(range_dtt_10y_1),
            RandomDistribution.uniform(range_dtt_10y_2),
            RandomDistribution.uniform(range_dtt_20y),
        ],
        [1, 1, 1],
    )
)

#######################
# String distributions

PRINTED_CHAR_MIN_CODE = ord("0")
PRINTED_CHAR_MAX_CODE = ord("~")

ascii_printable_chars = [
    chr(code) for code in range(PRINTED_CHAR_MIN_CODE, PRINTED_CHAR_MAX_CODE + 1)
]


def next_char(char: str, distance: int, min_char_code: int, max_char_code: int):
    char_code = ord(char)
    assert (
        min_char_code <= char_code <= max_char_code
    ), f'char_code "{char_code}" is out of range ({min_char_code}, {max_char_code})'
    number_of_chars = max_char_code - min_char_code + 1
    new_char_code = ((char_code - min_char_code + distance) % number_of_chars) + min_char_code
    assert (
        min_char_code <= new_char_code <= max_char_code
    ), f'new char code "{new_char_code}" is out of range'
    return chr(new_char_code)


def generate_str_by_distance(
    num_strings: int,
    seed_str: str,
    distance_distr_0: RandomDistribution,
    distance_distr_1: RandomDistribution,
    distance_distr_2: RandomDistribution,
    distance_distr_3: RandomDistribution,
):
    """
    Generate a set of unique strings with different string distances.

    The generation starts with a seed string 'seed_str', and each subsequent string is generated
    by producing the next character at each string position according to the distance generator
    'distance_distr_i' for the corresponding position.

    Given that the current histogram and CE implementation takes into account only the first 4
    characters, the length of the strings is limited to 4.
    """
    str_set = set()
    distances_0 = distance_distr_0.generate(num_strings)
    distances_1 = distance_distr_1.generate(num_strings)
    distances_2 = distance_distr_2.generate(num_strings)
    distances_3 = distance_distr_3.generate(num_strings)
    cur_str = seed_str
    str_set.add(cur_str)
    for i in range(1, num_strings):
        new_str = next_char(
            cur_str[0], distances_0[i], PRINTED_CHAR_MIN_CODE, PRINTED_CHAR_MAX_CODE
        )
        new_str += next_char(
            cur_str[1], distances_1[i], PRINTED_CHAR_MIN_CODE, PRINTED_CHAR_MAX_CODE
        )
        new_str += next_char(
            cur_str[2], distances_2[i], PRINTED_CHAR_MIN_CODE, PRINTED_CHAR_MAX_CODE
        )
        new_str += next_char(
            cur_str[3], distances_3[i], PRINTED_CHAR_MIN_CODE, PRINTED_CHAR_MAX_CODE
        )
        str_set.add(new_str)
        cur_str = new_str
    return list(str_set)


# Ranges of distances between string characters
range_int_1_1 = RangeGenerator(DataType.INTEGER, 1, 2, 1)
range_int_1_7 = RangeGenerator(DataType.INTEGER, 1, 8, 3)
range_int_6_12 = RangeGenerator(DataType.INTEGER, 6, 13, 3)
range_int_1_16 = RangeGenerator(DataType.INTEGER, 1, 20, 5)
range_int_20_30 = RangeGenerator(DataType.INTEGER, 20, 31, 3)
# Data distributions of ranges between string characters
d1 = RandomDistribution.uniform(range_int_1_1)
d2 = RandomDistribution.uniform(range_int_1_7)
d3 = RandomDistribution.uniform(range_int_6_12)
d4 = RandomDistribution.uniform(range_int_20_30)

# Sets of strings where characters at different positions have different distances
string_sets = {}
# 250 unique strings
string_sets["set_1112_250"] = generate_str_by_distance(250, "xxxx", d1, d1, d1, d2)
string_sets["set_2221_250"] = generate_str_by_distance(250, "azay", d2, d2, d3, d1)
string_sets["set_5555_250"] = generate_str_by_distance(250, "axbz", d4, d4, d4, d4)
# 1000 unique strings
string_sets["set_1112_1000"] = generate_str_by_distance(1000, "xxxx", d1, d1, d1, d2)
string_sets["set_2221_1000"] = generate_str_by_distance(1000, "azay", d2, d2, d3, d1)
string_sets["set_5555_1000"] = generate_str_by_distance(1000, "axbz", d4, d4, d4, d4)
# 10000 unique strings
string_sets["set_1112_10000"] = generate_str_by_distance(10000, "xxxx", d1, d1, d1, d2)
string_sets["set_2221_10000"] = generate_str_by_distance(10000, "azay", d2, d2, d3, d1)
string_sets["set_5555_10000"] = generate_str_by_distance(10000, "axbz", d4, d4, d4, d4)

# Weights with different variance. For instance if the smallest weight is 1, and the biggest weight is 5
# then some values in a choice distribution will be picked with at most 5 times higher probability.

# 5% variance in choice probability - all strings are chosen with almost the same probability.
weight_range_s = RangeGenerator(DataType.INTEGER, 95, 101, 1)
# 30% variance in choice probability
# weight_range_m = RangeGenerator(DataType.INTEGER, 65, 101, 2)
# 70% variance in choice probability
weight_range_l = RangeGenerator(DataType.INTEGER, 25, 101, 2)

weights = {}
weights["weight_unif_s"] = RandomDistribution.uniform(weight_range_s)
weights["weight_unif_l"] = RandomDistribution.uniform(weight_range_l)

# weights['weight_norm_s'] = RandomDistribution.normal(weight_range_s)
# weights['weight_norm_l'] = RandomDistribution.normal(weight_range_l)

# weights['chi2_s'] = RandomDistribution.noncentral_chisquare(weight_range_s)
# weights['chi2_l'] = RandomDistribution.noncentral_chisquare(weight_range_l)


def add_choice_distr(
    distr_set: Sequence[RandomDistribution],
    str_set: Sequence[str],
    weight_distr: RandomDistribution,
    v_name: str,
    w_name: str,
):
    distr = RandomDistribution.choice(str_set, weight_distr.generate(len(str_set)), v_name, w_name)
    distr_set.append(distr)


# String data distributions to be used for string generation

str_distributions = []

for set_name, cur_set in string_sets.items():
    for weight_name, cur_weight in weights.items():
        add_choice_distr(str_distributions, cur_set, cur_weight, set_name, weight_name)

#######################
# Array distributions

# array lenght distributions - they are all uniform
arr_len_dist_s = RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 6, 1))
arr_len_dist_m = RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 90, 110, 3))
arr_len_dist_l = RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 900, 1100, 10))


def add_array_distr(
    distr_set: Sequence[RandomDistribution],
    lengths_distr: RandomDistribution,
    value_distr: RandomDistribution,
):
    distr_set.append(ArrayRandomDistribution(lengths_distr, value_distr))


arr_distributions = []

# Arrays with integers
add_array_distr(arr_distributions, arr_len_dist_s, int_distributions[0])
add_array_distr(arr_distributions, arr_len_dist_m, int_distributions[0])
add_array_distr(arr_distributions, arr_len_dist_l, int_distributions[0])
add_array_distr(arr_distributions, arr_len_dist_s, int_distributions[10])
add_array_distr(arr_distributions, arr_len_dist_m, int_distributions[10])
add_array_distr(arr_distributions, arr_len_dist_l, int_distributions[10])

# Arrays with strings
add_array_distr(arr_distributions, arr_len_dist_s, str_distributions[1])
add_array_distr(arr_distributions, arr_len_dist_m, str_distributions[1])
add_array_distr(arr_distributions, arr_len_dist_l, str_distributions[1])
add_array_distr(arr_distributions, arr_len_dist_s, str_distributions[-1])
add_array_distr(arr_distributions, arr_len_dist_m, str_distributions[-1])
add_array_distr(arr_distributions, arr_len_dist_l, str_distributions[-1])

# 30% scalars, 70% arrays
arr_distributions.append(
    RandomDistribution.mixed([int_distributions[0], arr_distributions[0]], [0.3, 0.7])
)
arr_distributions.append(
    RandomDistribution.mixed([int_distributions[-1], arr_distributions[-1]], [0.3, 0.7])
)
# 70% scalars, 30% arrays
arr_distributions.append(
    RandomDistribution.mixed([int_distributions[0], arr_distributions[0]], [0.7, 0.3])
)
arr_distributions.append(
    RandomDistribution.mixed([int_distributions[-1], arr_distributions[-1]], [0.7, 0.3])
)

arr_zero_size = RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 0, 1, 1))
arr_empty_distr = ArrayRandomDistribution(arr_zero_size, int_distributions[0])

# 20% empty arrays
arr_distributions.append(
    RandomDistribution.mixed([arr_empty_distr, arr_distributions[2]], [0.2, 0.8])
)
# 80% empty arrays
arr_distributions.append(
    RandomDistribution.mixed([arr_empty_distr, arr_distributions[2]], [0.8, 0.2])
)

###############################
# Mixed data type distributions

mix_distributions = []

# Integers + strings
int_str_mix_1 = [int_distributions[0], str_distributions[0]]
int_str_mix_2 = [int_distributions_offset[7], str_distributions[-1]]

mix_distributions.append(RandomDistribution.mixed(children=int_str_mix_1, weight=[0.5, 0.5]))
mix_distributions.append(RandomDistribution.mixed(children=int_str_mix_2, weight=[0.5, 0.5]))

mix_distributions.append(RandomDistribution.mixed(children=int_str_mix_1, weight=[0.1, 0.9]))
mix_distributions.append(RandomDistribution.mixed(children=int_str_mix_1, weight=[0.9, 0.1]))
mix_distributions.append(RandomDistribution.mixed(children=int_str_mix_2, weight=[0.1, 0.9]))
mix_distributions.append(RandomDistribution.mixed(children=int_str_mix_2, weight=[0.9, 0.1]))

# Doubles and strings
dbl_ascii_range = RangeGenerator(
    DataType.DOUBLE, float(PRINTED_CHAR_MIN_CODE), float(PRINTED_CHAR_MAX_CODE), 0.01
)
ascii_double_range_distr = RandomDistribution.normal(dbl_ascii_range)

dbl_str_mix_1 = [ascii_double_range_distr, str_distributions[1]]
mix_distributions.append(RandomDistribution.mixed(children=dbl_str_mix_1, weight=[0.5, 0.5]))
mix_distributions.append(RandomDistribution.mixed(children=dbl_str_mix_1, weight=[0.1, 0.9]))
mix_distributions.append(RandomDistribution.mixed(children=dbl_str_mix_1, weight=[0.9, 0.1]))

dbl_str_mix_2 = [dbl_distributions[5], str_distributions[0]]
mix_distributions.append(RandomDistribution.mixed(children=dbl_str_mix_2, weight=[0.5, 0.5]))

dbl_str_mix_3 = [dbl_distributions[5], str_distributions[5]]
mix_distributions.append(RandomDistribution.mixed(children=dbl_str_mix_3, weight=[0.5, 0.5]))

# Doubles and/or strings and dates

dbl_str_dt_mix_1 = [ascii_double_range_distr, str_distributions[4], dt_distributions[0]]
mix_distributions.append(
    RandomDistribution.mixed(children=dbl_str_dt_mix_1, weight=[0.5, 0.5, 0.5])
)

str_dt_mix_1 = [str_distributions[0], dt_distributions[-1]]
mix_distributions.append(RandomDistribution.mixed(children=str_dt_mix_1, weight=[0.5, 0.5]))
str_dt_mix_2 = [str_distributions[-1], dt_distributions[0]]
mix_distributions.append(RandomDistribution.mixed(children=str_dt_mix_2, weight=[0.5, 0.5]))

################################################################################
# Collection templates
################################################################################
# In order to enable quicker Evergreen testing, and to reduce the size of the generated file
# that is committed to git, by default we generate only 100 and 1000 document collections.
# These are not sufficient for actual CE accuracy testing. Whenever one needs to estimate CE
# accuracy, they should generate larger datasets offline. To achieve this, set
# collection_cardinalities = [1000, 10000, 100000]
# Notice that such sizes result in several minutes load time on the JS test side.
collection_cardinalities = [500]

field_templates = [
    config.FieldTemplate(
        name=f"{str(dist)}", data_type=config.DataType.INTEGER, distribution=dist, indexed=False
    )
    for dist in int_distributions
]
field_templates += [
    config.FieldTemplate(
        name=f"{str(dist)}", data_type=config.DataType.STRING, distribution=dist, indexed=False
    )
    for dist in str_distributions
]
field_templates += [
    config.FieldTemplate(
        name=f"{str(dist)}", data_type=config.DataType.ARRAY, distribution=dist, indexed=False
    )
    for dist in arr_distributions
]
field_templates += [
    config.FieldTemplate(
        name=f"{str(dist)}", data_type=config.DataType.DOUBLE, distribution=dist, indexed=False
    )
    for dist in dbl_distributions
]
field_templates += [
    config.FieldTemplate(
        name=f"{str(dist)}", data_type=config.DataType.DATE, distribution=dist, indexed=False
    )
    for dist in dt_distributions
]
field_templates += [
    config.FieldTemplate(
        name=f"{str(dist)}", data_type=config.DataType.MIXDATA, distribution=dist, indexed=False
    )
    for dist in mix_distributions
]

ce_data = config.CollectionTemplate(
    name="ce_data",
    fields=field_templates,
    compound_indexes=[],
    cardinalities=collection_cardinalities,
)

################################################################################
# Database settings
################################################################################

database_config = config.DatabaseConfig(
    connection_string="mongodb://localhost",
    database_name="ce_accuracy_test",
    dump_path=Path("..", "..", "jstests", "query_golden", "libs", "data"),
    restore_from_dump=config.RestoreMode.NEVER,
    dump_on_exit=False,
)

################################################################################
# Data Generator settings
################################################################################

data_generator_config = config.DataGeneratorConfig(
    enabled=True,
    create_indexes=False,
    batch_size=10000,
    collection_templates=[ce_data],
    write_mode=config.WriteMode.REPLACE,
    collection_name_with_card=True,
)
