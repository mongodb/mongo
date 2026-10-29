# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

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
    str_field: Specification("A")


@dataclasses.dataclass
class TypesTest:
    float_field: Specification(float(1.1))
    int_field: Specification(1)
    str_field: Specification("A")
    bool_field: Specification(True)
    datetime_datetime_field: Specification(DATETIME)
    bson_datetime_ms_field: Specification(bson.datetime_ms.DatetimeMS(DATETIME))
    bson_timestamp_field: Specification(bson.timestamp.Timestamp(DATETIME, 123))
    bson_decimal128_field: Specification(bson.decimal128.Decimal128("1.1"))
    array_field: Specification([1, 2])
    obj_field: Specification(NestedObject)
    dict_field: Specification({"a": 1})
    enum_field: Specification(TestEnum.A)
    int_enum_field: Specification(TestIntEnum.A)
    null_field: Specification(None)
    missing_field: Specification(MISSING)
