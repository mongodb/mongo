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
"""
Configuration of distributions used to generate collections from templates.

They used in collection templates defined in json.config.
"""

from importlib.metadata import distributions
from random_generator import RangeGenerator, DataType, RandomDistribution

__ALL__ = ['distributions']

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
