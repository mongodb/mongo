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

from pathlib import Path
import random
from typing import Sequence
import config
from random_generator import RangeGenerator, DataType, RandomDistribution, ArrayRandomDistribution

__all__ = ['database_config', 'data_generator_config']

################################################################################
# Data distributions
################################################################################

# Ranges

# 1K unique numbers with different distances
range_int_1000_1 = RangeGenerator(DataType.INTEGER, 0, 1000, 1)
range_int_1000_10 = RangeGenerator(DataType.INTEGER, 0, 10000, 10)
range_int_1000_100 = RangeGenerator(DataType.INTEGER, 0, 100000, 100)
range_int_1000_1000 = RangeGenerator(DataType.INTEGER, 0, 1000000, 1000)
# 10K unique numbers with different distances
range_int_10000_1 = RangeGenerator(DataType.INTEGER, 0, 10000, 1)
range_int_10000_10 = RangeGenerator(DataType.INTEGER, 0, 100000, 10)
range_int_10000_100 = RangeGenerator(DataType.INTEGER, 0, 1000000, 100)
range_int_10000_1000 = RangeGenerator(DataType.INTEGER, 0, 10000000, 1000)
int_ranges = [
    range_int_1000_1, range_int_1000_10, range_int_1000_100, range_int_1000_1000, range_int_10000_1,
    range_int_10000_10, range_int_10000_100, range_int_10000_1000
]

# Integer distributions
int_distributions = {}
for int_range in int_ranges:
    int_distributions[
        f'uniform_int_{int_range.interval_end - int_range.interval_begin}_{int_range.step}'] = RandomDistribution.uniform(
            int_range)
    int_distributions[
        f'normal_int_{int_range.interval_end - int_range.interval_begin}_{int_range.step}'] = RandomDistribution.normal(
            int_range)
    int_distributions[
        f'chi2_int_{int_range.interval_end - int_range.interval_begin}_{int_range.step}'] = RandomDistribution.noncentral_chisquare(
            int_range)

# Mixes of distributions with different NDV and value distances

unf_int_mix_1 = [
    int_distributions['uniform_int_10000_10'], int_distributions['uniform_int_100000_100'],
    int_distributions['uniform_int_10000000_1000']
]
int_distributions['mixed_int_uniform_1'] = RandomDistribution.mixed(children=unf_int_mix_1,
                                                                    weight=[1, 1, 1])

unf_norm_int_mix_1 = [
    int_distributions['uniform_int_1000_1'], int_distributions['normal_int_100000_100'],
    int_distributions['normal_int_10000000_1000']
]
int_distributions['mixed_int_unf_norm_1'] = RandomDistribution.mixed(children=unf_norm_int_mix_1,
                                                                     weight=[1, 1, 1])

unf_norm_chi_int_mix_1 = [
    int_distributions['uniform_int_10000_10'], int_distributions['uniform_int_1000000_100'],
    int_distributions['normal_int_10000_10'], int_distributions['normal_int_1000000_100'],
    int_distributions['chi2_int_10000_10'], int_distributions['chi2_int_10000000_1000']
]
int_distributions['mixed_int_unf_norm_chi_1'] = RandomDistribution.mixed(
    children=unf_norm_chi_int_mix_1, weight=[1, 1, 1, 1, 1, 1])

unf_norm_chi_int_mix_2 = [
    int_distributions['uniform_int_10000_10'],
    int_distributions['normal_int_10000_10'],
    int_distributions['uniform_int_1000000_100'],
    int_distributions['normal_int_1000000_100'],
    int_distributions['chi2_int_1000000_100'],
]
int_distributions['mixed_int_unf_norm_chi_2'] = RandomDistribution.mixed(
    children=unf_norm_chi_int_mix_2, weight=[1, 1, 1, 1, 1])

# String distributions

PRINTED_CHAR_MIN_CODE = 32
PRINTED_CHAR_MAX_CODE = 126

ascii_printable_chars = [
    chr(code) for code in range(PRINTED_CHAR_MIN_CODE, PRINTED_CHAR_MAX_CODE + 1)
]


def next_char(char: str, distance: int, min_char_code: int, max_char_code: int):
    char_code = ord(char)
    assert (min_char_code <= char_code <= max_char_code), "char is out of range"
    number_of_chars = max_char_code - min_char_code + 1
    new_char_code = ((char_code - min_char_code + distance) % number_of_chars) + min_char_code
    assert (min_char_code <= new_char_code <= max_char_code), "new char is out of range"
    return chr(new_char_code)


def generate_str_by_distance(num_strings: int, seed_str: str, distance_distr_0: RandomDistribution,
                             distance_distr_1: RandomDistribution,
                             distance_distr_2: RandomDistribution,
                             distance_distr_3: RandomDistribution):
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
        new_str = next_char(cur_str[0], distances_0[i], PRINTED_CHAR_MIN_CODE,
                            PRINTED_CHAR_MAX_CODE)
        new_str += next_char(cur_str[1], distances_1[i], PRINTED_CHAR_MIN_CODE,
                             PRINTED_CHAR_MAX_CODE)
        new_str += next_char(cur_str[2], distances_2[i], PRINTED_CHAR_MIN_CODE,
                             PRINTED_CHAR_MAX_CODE)
        new_str += next_char(cur_str[3], distances_3[i], PRINTED_CHAR_MIN_CODE,
                             PRINTED_CHAR_MAX_CODE)
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
# 33 unique strings
string_sets['set_1112_33'] = generate_str_by_distance(33, 'xxxx', d1, d1, d1, d2)
string_sets['set_2221_33'] = generate_str_by_distance(33, 'aZa3', d2, d2, d3, d1)
string_sets['set_5555_33'] = generate_str_by_distance(33, 'axbz', d4, d4, d4, d4)
# 1000 unique strings
string_sets['set_1112_1000'] = generate_str_by_distance(1000, 'xxxx', d1, d1, d1, d2)
string_sets['set_2221_1000'] = generate_str_by_distance(1000, 'aZa3', d2, d2, d3, d1)
string_sets['set_5555_1000'] = generate_str_by_distance(1000, 'axbz', d4, d4, d4, d4)

# Weights with different variance. For instance if the smallest weight is 1, and the biggest weight is 5
# then some values in a choice distribution will be picked with at most 5 times higher probability.

# 5% variance in choice probability - all strings are chosen with almost the same probability.
weight_range_s = RangeGenerator(DataType.INTEGER, 95, 101, 1)
# 30% variance in choice probability
# weight_range_m = RangeGenerator(DataType.INTEGER, 65, 101, 2)
# 70% variance in choice probability
weight_range_l = RangeGenerator(DataType.INTEGER, 25, 101, 2)

weights = {}
weights['norm_s'] = RandomDistribution.normal(weight_range_s)
#weights['norm_m'] = RandomDistribution.normal(weight_range_m)
weights['norm_l'] = RandomDistribution.normal(weight_range_l)
weights['chi2_s'] = RandomDistribution.noncentral_chisquare(weight_range_s)
#weights['chi2_m'] = RandomDistribution.noncentral_chisquare(weight_range_m)
weights['chi2_l'] = RandomDistribution.noncentral_chisquare(weight_range_l)


def make_choice_distr(str_set: Sequence[str], weight_distr: RandomDistribution):
    return RandomDistribution.choice(str_set, weight_distr.generate(len(str_set)))


# String data distributions to be used for string generation

str_distributions = {}

for set_name, cur_set in string_sets.items():
    for weight_name, weight in weights.items():
        str_distributions[f'choice_str_{set_name}_{weight_name}'] = make_choice_distr(
            cur_set, weight)

################################################################################
# Collection templates
################################################################################
# In order to enable quicker Evergreen testing, and to reduce the size of the generated file
# that is committed to git, by default we generate only 100 and 1000 document collections.
# These are not sufficient for actual CE accuracy testing. Whenever one needs to estimate CE
# accuracy, they should generate larger datasets offline. To achieve this, set
# collection_cardinalities = [100, 1000, 10000, 100000]
# Notice that such sizes result in several minutes load time on the JS test side.
collection_cardinalities = [100, 1000]

field_templates = [
    config.FieldTemplate(name=f'{dist_name}', data_type=config.DataType.INTEGER, distribution=dist,
                         indexed=False) for dist_name, dist in int_distributions.items()
]
field_templates += [
    config.FieldTemplate(name=f'{dist_name}', data_type=config.DataType.STRING, distribution=dist,
                         indexed=False) for dist_name, dist in str_distributions.items()
]

ce_data = config.CollectionTemplate(name="ce_data", fields=field_templates, compound_indexes=[],
                                    cardinalities=collection_cardinalities)

################################################################################
# Database settings
################################################################################

database_config = config.DatabaseConfig(
    connection_string='mongodb://localhost', database_name='ce_accuracy_test', dump_path=Path(
        '..', '..', 'jstests', 'query_golden', 'libs', 'data'),
    restore_from_dump=config.RestoreMode.NEVER, dump_on_exit=False)

################################################################################
# Data Generator settings
################################################################################

data_generator_config = config.DataGeneratorConfig(
    enabled=True, create_indexes=False, batch_size=10000, collection_templates=[ce_data],
    write_mode=config.WriteMode.REPLACE, collection_name_with_card=True)
