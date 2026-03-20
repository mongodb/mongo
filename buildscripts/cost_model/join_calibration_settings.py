# Copyright (C) 2026-present MongoDB, Inc.
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
"""Configuration for Join Cost Model Calibration.

Two collections (join_coll_1, join_coll_2) with identical schema:
    - unique:        Sequential unique integers in [1, COLLECTION_CARDINALITY]
    - random:        Random unique integers in [1, COLLECTION_CARDINALITY]
    - uniform_16:    Uniform random integers in [1, 16]
    - uniform_256:   Uniform random integers in [1, 256]
    - uniform_4k:    Uniform random integers in [1, 4096]
    - uniform_64k:   Uniform random integers in [1, 65536]
    - string_filler: Random string to increase document size

Indexes (on join_coll_2 only, enabling INLJ plans):
    {unique: 1}, {uniform_16: 1}, {uniform_256: 1}, {uniform_4k: 1}, {uniform_64k: 1}
"""

from __future__ import annotations

import os

import config
from random_generator import DataType, RandomDistribution, RangeGenerator, StringRandomDistribution

COLLECTION_CARDINALITY = 100_000
DEFAULT_STRING_FILLER_LENGTH = 900
LARGE_STRING_FILLER_LENGTH = 3_000


def create_join_collection_template(
    name: str,
    string_filler_length: int = DEFAULT_STRING_FILLER_LENGTH,
    create_indexes: bool = False,
) -> config.CollectionTemplate:
    """Create a collection template for join calibration."""
    return config.CollectionTemplate(
        name=name,
        fields=[
            config.FieldTemplate(
                name="unique",
                data_type=DataType.INTEGER,
                distribution=RandomDistribution.sequential(
                    RangeGenerator(DataType.INTEGER, 1, COLLECTION_CARDINALITY + 1)
                ),
                indexed=create_indexes,
            ),
            config.FieldTemplate(
                name="random",
                data_type=DataType.INTEGER,
                distribution=RandomDistribution.permutation(
                    RangeGenerator(DataType.INTEGER, 1, COLLECTION_CARDINALITY + 1)
                ),
                indexed=False,
            ),
            config.FieldTemplate(
                name="uniform_16",
                data_type=DataType.INTEGER,
                distribution=RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 17)),
                indexed=create_indexes,
            ),
            config.FieldTemplate(
                name="uniform_256",
                data_type=DataType.INTEGER,
                distribution=RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 257)),
                indexed=create_indexes,
            ),
            config.FieldTemplate(
                name="uniform_4k",
                data_type=DataType.INTEGER,
                distribution=RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 4097)),
                indexed=create_indexes,
            ),
            config.FieldTemplate(
                name="uniform_64k",
                data_type=DataType.INTEGER,
                distribution=RandomDistribution.uniform(RangeGenerator(DataType.INTEGER, 1, 65537)),
                indexed=create_indexes,
            ),
            config.FieldTemplate(
                name="string_filler",
                data_type=DataType.STRING,
                distribution=StringRandomDistribution(string_filler_length, pool_size=1000),
                indexed=False,
            ),
        ],
        compound_indexes=[],
        cardinalities=[COLLECTION_CARDINALITY],
    )


join_coll_1 = create_join_collection_template(
    name="join_coll_1",
    create_indexes=False,
)
join_coll_1_large = create_join_collection_template(
    name="join_coll_1_large",
    string_filler_length=LARGE_STRING_FILLER_LENGTH,
    create_indexes=False,
)
join_coll_2 = create_join_collection_template(
    name="join_coll_2",
    create_indexes=True,
)
join_coll_2_large = create_join_collection_template(
    name="join_coll_2_large",
    string_filler_length=LARGE_STRING_FILLER_LENGTH,
    create_indexes=True,
)

join_database = config.DatabaseConfig(
    connection_string=os.getenv("MONGODB_URI", "mongodb://localhost"),
    database_name="join_calibration",
    dump_path="~/mongo/buildscripts/cost_model",
    restore_from_dump=config.RestoreMode.NEVER,
    dump_on_exit=False,
)

join_data_generator = config.DataGeneratorConfig(
    enabled=True,
    create_indexes=True,
    batch_size=COLLECTION_CARDINALITY,
    collection_templates=[join_coll_1, join_coll_1_large, join_coll_2, join_coll_2_large],
    write_mode=config.WriteMode.REPLACE,
    collection_name_with_card=False,
)
