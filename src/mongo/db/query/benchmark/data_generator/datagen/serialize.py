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

"""Utilities for serializing generated documents."""

import json
from enum import Enum

from datagen.statistics import *
from datagen.util import MISSING


def serialize_doc(obj: dict):
    """Recursively serializes special types within a dictionary for insertion."""

    return {k: serialize(v) for k, v in obj.items() if v != MISSING}


def serialize(v):
    if isinstance(v, dict):
        return serialize_doc(v)
    elif isinstance(v, frozenset):
        # Some dicts arrive here as frozensets, so convert them back to dicts
        return serialize_doc(dict(v))
    elif isinstance(v, Enum):
        return repr(v)
    else:
        return v


class StatisticsEncoder(json.JSONEncoder):
    def default(self, o):
        if isinstance(o, StatisticsRegister):
            return o.statistics
        if isinstance(o, FieldStatistic):
            result = dict(
                missing_count=o.missing_count,
                null_count=o.null_count,
                is_multikey=o.multikey,
            )
            if o.statistic_by_scalar_type:
                result["types"] = {
                    SUPPORTED_SCALAR_TYPES[k]: v for k, v in o.statistic_by_scalar_type.items()
                }
            if o.nested_object:
                result["nested_object"] = o.nested_object
            return result

        if isinstance(o, FieldStatisticByScalarType):
            return dict(
                min=serialize(o.min),
                max=serialize(o.max),
                unique=[serialize(v) for v in o.unique],
            )
        if result := serialize_supported(o):
            return result
        # Let the base class default method raise the TypeError
        return super(StatisticsEncoder, self).default(o)
