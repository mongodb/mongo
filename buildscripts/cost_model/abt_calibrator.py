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
"""Calibrate ABT nodes."""

from __future__ import annotations
from typing import Sequence
from config import AbtCalibratorConfig
from database_instance import DatabaseInstance
from cost_estimator import estimate
from parameters_extractor import extract_parameters

__all__ = ['calibrate']


def calibrate(config: AbtCalibratorConfig, database: DatabaseInstance, abt_types: Sequence[str]):
    """Main entry-point for ABT calibration."""

    if not config.enabled:
        return {}

    result = {}
    stats = extract_parameters(config, database, abt_types)
    for abt, abt_stats in stats.items():
        result[abt] = estimate(abt_stats, config.test_size, config.trace)
    return result
