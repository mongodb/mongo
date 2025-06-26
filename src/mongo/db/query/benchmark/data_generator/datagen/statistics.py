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
        self.unique = set()

    def register(self, field_value):
        if self.min is None or field_value < self.min:
            self.min = field_value
        if self.max is None or field_value > self.max:
            self.max = field_value
        if len(self.unique) < SAMPLE_LIMIT:
            self.unique.add(field_value)


SAMPLE_LIMIT = 1000

SUPPORTED_SCALAR_TYPES = {
    float.__name__: "dbl",
    str.__name__: "str",
    bson.objectid.ObjectId.__name__: "oid",
    bool.__name__: "bool",
    datetime.date.__name__: "dt",
    int.__name__: "int",
    datetime.time.__name__: "ts",
    bson.decimal128.Decimal128.__name__: "dec",
}


def serialize_supported(v):
    if isinstance(v, datetime.date):
        return str(v)
    if issubclass(type(v), Enum):
        return repr(v)
    return None
