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
Generates values to pick over using a distribution.
"""

import dataclasses
import datetime
import typing

import numpy as np

TVar = typing.TypeVar("TVar", str, int, float, datetime.datetime)


@dataclasses.dataclass
class RangeGenerator(typing.Generic[TVar]):
    """Produces a sequence of non-random data for the given interval and step."""

    data_type: type
    interval_begin: TVar
    interval_end: TVar
    step: int = 1
    ndv: int = -1

    def __post_init__(self):
        assert type(self.interval_begin) == type(self.interval_end), (
            "Interval ends must of the same type."
        )
        if type(self.interval_begin) == int or type(self.interval_begin) == float:
            self.ndv = round((self.interval_end - self.interval_begin) / self.step)
        elif type(self.interval_begin) == datetime.datetime:
            begin_ts = self.interval_begin.timestamp()
            end_ts = self.interval_end.timestamp()
            self.ndv = round((end_ts - begin_ts) / self.step)

    def generate(self) -> typing.Sequence[TVar]:
        """Generate the range."""

        gen_range_dict = {
            "str": ansi_range,
            "int": range,
            # The arange function produces equi-distant values which is too regular for CE testing.
            # It is left here as a possible way of generating doubles.
            # DataType.DOUBLE: np.arange
            "float": double_range,
            "datetime": datetime_range,
        }

        gen_range = gen_range_dict.get(self.data_type.__name__)
        if gen_range is None:
            raise ValueError(f"Unsupported data type: {self.data_type.__name__}")

        return list(gen_range(self.interval_begin, self.interval_end, self.step))

    def __str__(self):
        # TODO: for now skip NDV from the name to make it shorter.
        # ndv_str = "_" if self.ndv <= 0 else f'_{self.ndv}_'
        begin_str = (
            str(self.interval_begin.date())
            if isinstance(self.interval_begin, datetime.datetime)
            else str(self.interval_begin)
        )
        end_str = (
            str(self.interval_end.date())
            if isinstance(self.interval_end, datetime.datetime)
            else str(self.interval_end)
        )

        str_rep = f"{str(self.data_type)}_{begin_str}-{end_str}-{self.step}"
        # Remove dots and spaces from field names.
        str_rep = str_rep.replace(".", ",")
        str_rep = str_rep.replace(" ", "_")
        return str_rep


def double_range(begin: float, end: float, step: float = 1.0):
    """Produce a sequence of double values within a range."""

    return np.random.default_rng().uniform(begin, end, round((end - begin) / step))


def ansi_range(begin: str, end: str, step: int = 1):
    """Produces a sequence of string from begin to end."""

    alphabet_size = 28
    non_alpha_char = "_"

    def ansi_to_int(data: str) -> int:
        res = 0
        for char in data.lower():
            res = res * alphabet_size
            if "a" <= char <= "z":
                res += ord(char) - ord("a") + 1
            else:
                res += alphabet_size - 1
        return res

    def int_to_ansi(data: int) -> str:
        result = []
        while data != 0:
            data, remainder = divmod(data, alphabet_size)
            if remainder == alphabet_size - 1:
                char = non_alpha_char
            else:
                char = chr(remainder + ord("a") - 1)
            result.append(char)
        result.reverse()
        return "".join(result)

    def get_common_prefix_len(s1: str, s2: str):
        index = 0
        for c1, c2 in zip(s1, s2):
            if c1 == c2:
                index += 1
            else:
                break

        return index

    prefix_len = get_common_prefix_len(begin, end)
    if prefix_len > 0:
        prefix = begin[:prefix_len]
        begin = begin[prefix_len:]
        end = end[prefix_len:]
    for number in range(ansi_to_int(begin), ansi_to_int(end), step):
        if prefix_len == 0:
            yield int_to_ansi(number)
        else:
            yield f"{prefix}{int_to_ansi(number)}"


def datetime_range(begin: datetime.datetime, end: datetime.datetime, step: int = 60):
    begin_ts = begin.timestamp()
    end_ts = end.timestamp()
    num_values = round((end_ts - begin_ts) / step)
    assert num_values >= 1, "Datetime range must be bigger than the step."
    for _ in range(0, num_values):
        random_ts = np.random.randint(begin_ts, end_ts)
        yield datetime.datetime.fromtimestamp(random_ts)
    # random_dates = [datetime.datetime.fromtimestamp(random_ts) for random_ts in default_random().sample(range(int(begin_ts), int(end_ts)), num_values)]
    # return random_dates
