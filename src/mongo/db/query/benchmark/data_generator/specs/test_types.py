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

"""
Example collection spec used for testing individual data types.
"""

import dataclasses
import datetime
import enum

import bson
from datagen.util import MISSING, Specification

DATETIME = datetime.datetime(2024, 1, 1, 12, 11, 10, tzinfo=datetime.timezone.utc)


class TestEnum(enum.Enum):
    A = enum.auto()

    def __lt__(self, other):
        return self.value < other.value

    def __repr__(self) -> str:
        return self.name

class TestIntEnum(enum.IntEnum):
    A = enum.auto()

    def __repr__(self) -> str:
        return str(self.value)

@dataclasses.dataclass
class NestedObject:
    str_field: Specification(str, source=lambda fkr: "A")

@dataclasses.dataclass
class TypesTest:
    float_field: Specification(float, source=lambda fkr: float(1.1))
    int_field: Specification(int, source=lambda fkr: 1)
    str_field: Specification(str, source=lambda fkr: "A")
    bool_field: Specification(bool, source=lambda fkr: True)
    datetime_datetime_field: Specification(datetime.datetime,
                                           source=lambda fkr: DATETIME)
    bson_datetime_ms_field: Specification(
        datetime, source=lambda fkr: bson.datetime_ms.DatetimeMS(DATETIME))
    bson_timestamp_field: Specification(
        bson.timestamp.Timestamp,
        source=lambda fkr: bson.timestamp.Timestamp(DATETIME, 123))
    bson_decimal128_field: Specification(
        bson.decimal128.Decimal128,
        source=lambda fkr: bson.decimal128.Decimal128("1.1"))
    array_field: Specification(list, source=lambda fkr: [1, 2])
    obj_field: Specification(NestedObject)
    dict_field: Specification(dict, source=lambda fkr: {'a': 1})
    enum_field: Specification(TestEnum, source=lambda fkr: TestEnum.A)
    int_enum_field: Specification(TestIntEnum,
                                  source=lambda fkr: TestIntEnum.A)
    null_field: Specification(type(None), source=lambda fkr: None)
    missing_field: Specification(type(None), source=lambda fkr: MISSING)
