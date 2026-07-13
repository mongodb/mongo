# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

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
