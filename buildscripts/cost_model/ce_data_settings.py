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

# Various integer distributions
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
    config.FieldTemplate(name=f'{dist}', data_type=config.DataType.INTEGER,
                         distribution=int_distributions[dist], indexed=True)
    for dist in int_distributions
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
