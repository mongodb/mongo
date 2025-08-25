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

"""Statistics gathering."""

import contextlib
import datetime
import decimal
from enum import Enum

import bson
import datagen.util


class StatisticsRegister:
    def __init__(self):
        self.statistics = {}
        self.current_path = []

    def register_fields(self, fields: dict):
        fs = self.statistics
        for component in self.current_path:
            if component not in fs:
                fs[component] = FieldStatistic()
            fs = fs[component].nested_object
        for f, v in fields.items():
            if f not in fs:
                fs[f] = FieldStatistic()
            fs[f].register(v)

    @contextlib.contextmanager
    def path_cm(self, field):
        try:
            self.current_path.append(field)
            yield None
        finally:
            self.current_path.pop()


class FieldStatistic:
    def __init__(self):
        self.missing_count = 0
        self.null_count = 0
        self.multikey = False
        self.statistic_by_scalar_type = {}
        self.nested_object = {}

    def register(self, field_value):
        if isinstance(field_value, list):
            self.multikey = True
            for value in field_value:
                self.register(value)
        elif isinstance(field_value, datagen.util.SpecialValue):
            self.missing_count += 1
        elif field_value is None:
            self.null_count += 1
        else:
            if issubclass(type(field_value), Enum):
                type_name = "str"
            else:
                type_name = type(field_value).__name__
            if type_name in SUPPORTED_SCALAR_TYPES:
                if type_name not in self.statistic_by_scalar_type:
                    self.statistic_by_scalar_type[type_name] = FieldStatisticByScalarType()
                self.statistic_by_scalar_type[type_name].register(field_value)


class FieldStatisticByScalarType:

    def __init__(self):
        self.min = None
        self.max = None
        # Representations used for comparison purposes
        # in case the original type does not suport
        # proper comparisons.
        self.min_cmp = None
        self.max_cmp = None
        self.unique = set()

    def register(self, field_value):
        if isinstance(field_value, bson.decimal128.Decimal128):
            # Python does not consider Decimal128 hashable or comparable, so
            # we can not add it to a set() or find the min/max without converting
            # it to decimal.Decimal.
            field_value = field_cmp = field_value.to_decimal()
        elif isinstance(field_value, dict):
            # dicts not support comparison, so compare sorted(.items()) instead.
            field_cmp = sorted(field_value.items())
        else:
            field_cmp = field_value

        if self.min is None or field_cmp < self.min_cmp:
            self.min = field_value
            self.min_cmp = field_cmp
        if self.max is None or field_cmp > self.max_cmp:
            self.max = field_value
            self.max_cmp = field_cmp

        if len(self.unique) < SAMPLE_LIMIT:
            if isinstance(field_value, dict):
                # dicts are not hashable/comparable, so
                # temporarily convert them to a frozenset
                # in order to be able to insert them in a set()
                self.unique.add(frozenset(field_value.items()))
            else:
                self.unique.add(field_value)


SAMPLE_LIMIT = 1000

SUPPORTED_SCALAR_TYPES = {
    float.__name__: "dbl",
    str.__name__: "str",
    bson.objectid.ObjectId.__name__: "oid",
    bool.__name__: "bool",
    datetime.datetime.__name__: "dt",
    bson.datetime_ms.DatetimeMS.__name__: "dt_ms",
    bson.timestamp.Timestamp.__name__: "ts",
    int.__name__: "int",
    bson.decimal128.Decimal128.__name__: "dec",
    dict.__name__: "obj",
    frozenset.__name__: "obj",
}


def serialize_supported(v):
    if isinstance(v, datetime.datetime):
        return v.isoformat()
    elif isinstance(v, (bson.decimal128.Decimal128, decimal.Decimal)):
        return str(v)
    elif isinstance(v,
                    (bson.datetime_ms.DatetimeMS, bson.timestamp.Timestamp)):
        return v.as_datetime().replace(tzinfo=datetime.timezone.utc).timestamp()
    elif issubclass(type(v), Enum):
        # We expect that the Enum will have a __repr__ method that returns
        # something that can be inserted into MongoDB
        return repr(v)
    return None
