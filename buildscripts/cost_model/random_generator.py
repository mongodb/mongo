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
"""Random data generator of various distributions."""

from __future__ import annotations

from ctypes import Union
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from itertools import chain
from typing import Generic, Sequence, TypeVar

import numpy as np

__all__ = ["RangeGenerator", "DataType", "RandomDistribution"]

TVar = TypeVar("TVar", str, int, float, datetime)


class DataType(Enum):
    """MongoDB data types of collection fields. Ordered according to BSON type order."""

    DOUBLE = 1
    STRING = 2
    OBJECT = 3
    ARRAY = 4
    OBJECTID = 7
    BOOLEAN = 8
    DATE = 9
    NULL = 10
    INTEGER = 16  # Both 32 and 64 bit ints
    TIMESTAMP = 17
    DECIMAL128 = 19
    MIXDATA = 42

    def __str__(self):
        typenames = {
            DataType.DOUBLE: "dbl",
            DataType.STRING: "str",
            DataType.OBJECT: "obj",
            DataType.ARRAY: "arr",
            DataType.OBJECTID: "oid",
            DataType.BOOLEAN: "bool",
            DataType.DATE: "dt",
            DataType.NULL: "null",
            DataType.INTEGER: "int",
            DataType.TIMESTAMP: "ts",
            DataType.DECIMAL128: "dec",
            DataType.MIXDATA: "mixdata",
        }
        return typenames[self]


@dataclass
class RangeGenerator(Generic[TVar]):
    """Produces a sequence of non-random data for the given interval and step."""

    data_type: DataType
    interval_begin: TVar
    interval_end: TVar
    step: int = 1
    ndv: int = -1

    def __post_init__(self):
        assert type(self.interval_begin) == type(
            self.interval_end
        ), "Interval ends must of the same type."
        if type(self.interval_begin) == int or type(self.interval_begin) == float:
            self.ndv = round((self.interval_end - self.interval_begin) / self.step)
        elif type(self.interval_begin) == datetime:
            begin_ts = self.interval_begin.timestamp()
            end_ts = self.interval_end.timestamp()
            self.ndv = round((end_ts - begin_ts) / self.step)

    def generate(self) -> Sequence[TVar]:
        """Generate the range."""

        gen_range_dict = {
            DataType.STRING: ansi_range,
            DataType.INTEGER: range,
            # The arange function produces equi-distant values which is too regular for CE testing.
            # It is left here as a possible way of generating doubles.
            # DataType.DOUBLE: np.arange
            DataType.DOUBLE: double_range,
            DataType.DATE: datetime_range,
        }

        gen_range = gen_range_dict.get(self.data_type)
        if gen_range is None:
            raise ValueError(f"Unsupported data type: {self.data_type}")

        return list(gen_range(self.interval_begin, self.interval_end, self.step))

    def __str__(self):
        # TODO: for now skip NDV from the name to make it shorter.
        # ndv_str = "_" if self.ndv <= 0 else f'_{self.ndv}_'
        begin_str = (
            str(self.interval_begin.date())
            if isinstance(self.interval_begin, datetime)
            else str(self.interval_begin)
        )
        end_str = (
            str(self.interval_end.date())
            if isinstance(self.interval_end, datetime)
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


def datetime_range(begin: datetime, end: datetime, step: int = 60):
    begin_ts = begin.timestamp()
    end_ts = end.timestamp()
    num_values = round((end_ts - begin_ts) / step)
    assert num_values >= 1, "Datetime range must be bigger than the step."
    for _ in range(0, num_values):
        random_ts = np.random.randint(begin_ts, end_ts)
        yield datetime.fromtimestamp(random_ts)
    # random_dates = [datetime.fromtimestamp(random_ts) for random_ts in random.sample(range(int(begin_ts), int(end_ts)), num_values)]
    # return random_dates


class DistributionType(Enum):
    """An enum of distributions supported by Random Data Generator."""

    CHOICE = 0
    NORMAL = 1
    CHI2 = 2  # NONCENTRAL_CHISQUARE
    UNIFORM = 3
    MIXDIST = 4

    def __str__(self):
        return self.name.lower()


_rng = np.random.default_rng()


@dataclass
class RandomDistribution:
    """Produces random sequence of the specified values with the specified distribution."""

    distribution_type: DistributionType
    values: Union[Sequence[TVar], RangeGenerator]
    weights: Union[Sequence[float], None]
    values_name: str = ""
    weights_name: str = ""

    def __str__(self):
        def print_values(vals):
            if isinstance(vals, RangeGenerator):
                return str(vals)
            elif isinstance(vals[0], RandomDistribution):
                # Must be a mixed distribution
                res = ""
                for distr in vals:
                    res += f"{str(distr)}_"
                return res
            else:
                # All values are of the same type because of how RangeGenerator works
                return f"{type(vals[0]).__name__}_{min(vals)}_{max(vals)}_{len(vals)}"

        range_str = ""
        if hasattr(self, "values"):
            range_str = print_values(self.values)
        if self.values_name != "":
            range_str += f"_{self.values_name}"
        if self.weights_name != "":
            range_str += f"_{self.weights_name}"

        distr_str = f"{str(self.distribution_type)}_{range_str}"
        return distr_str

    @staticmethod
    def choice(
        values: Sequence[TVar],
        weights: Union[Sequence[float], RangeGenerator],
        v_name: str = "",
        w_name: str = "",
    ):
        """Create choice distribution."""
        return RandomDistribution(
            distribution_type=DistributionType.CHOICE,
            values=values,
            weights=weights,
            values_name=v_name,
            weights_name=w_name,
        )

    @staticmethod
    def normal(values: Union[Sequence[TVar], RangeGenerator]):
        """Create normal distribution."""
        return RandomDistribution(
            distribution_type=DistributionType.NORMAL, values=values, weights=None
        )

    @staticmethod
    def noncentral_chisquare(values: Union[Sequence[TVar], RangeGenerator]):
        """Create Non Central Chi2 distribution."""
        return RandomDistribution(
            distribution_type=DistributionType.CHI2, values=values, weights=None
        )

    @staticmethod
    def uniform(values: Union[Sequence[TVar], RangeGenerator]):
        """Create uniform distribution."""
        return RandomDistribution(
            distribution_type=DistributionType.UNIFORM, values=values, weights=None
        )

    @staticmethod
    def mixed(
        children: Sequence[RandomDistribution], weight: Union[Sequence[float], RangeGenerator]
    ):
        """Create mixed distribution."""
        return RandomDistribution(
            distribution_type=DistributionType.MIXDIST, values=children, weights=weight
        )

    def generate(self, size: int) -> Sequence[TVar]:
        """Generate random data sequence of the given size."""

        if isinstance(self.values, RangeGenerator):
            values = self.values.generate()
        else:
            values = self.values

        if isinstance(self.weights, RangeGenerator):
            weights = self.weights.generate()
        else:
            weights = self.weights

        if weights is not None:
            weights_sum = sum(weights)
            probs = [p / weights_sum for p in weights]
        else:
            probs = None

        if probs is not None and len(probs) != len(values):
            raise ValueError(f"values and probs must be the same size: {probs} !! {values}")

        if len(values) == 0:
            raise ValueError(f"Values cannot be empty: {self.values}")

        generators = {
            DistributionType.CHOICE: RandomDistribution._choice,
            DistributionType.NORMAL: RandomDistribution._normal,
            DistributionType.CHI2: RandomDistribution._noncentral_chisquare,
            DistributionType.UNIFORM: RandomDistribution._uniform,
            DistributionType.MIXDIST: RandomDistribution._mixed,
        }

        gen = generators.get(self.distribution_type)
        if gen is None:
            raise ValueError(f"Unsupported distribution type: {self.distribution_type}")

        return gen(size, values, probs)

    def get_values(self):
        """Return a list of values used to generate a random sequence."""
        if self.distribution_type == DistributionType.MIXDIST:
            result = []
            for child in self.values:
                result.append(child.get_values())
            return list(chain.from_iterable(result))

        if isinstance(self.values, RangeGenerator):
            return self.values.generate()

        return self.values

    @staticmethod
    def _choice(size: int, values: Sequence[TVar], probs: Sequence[float]):
        if probs is None:
            raise ValueError("props must be specified for choice distribution")
        return [val.item() for val in _rng.choice(a=values, size=size, p=probs)]

    @staticmethod
    def _normal(size: int, values: Sequence[TVar], _: Sequence[float]):
        # In according to the 68-95-99.7 rule 99.7% of values lie within three standard deviations of the mean.
        # Therefore, if we define stddev as `len(values) / 6` 99.7% of the values will lie within our `values` array bounds.
        # We define stddev as `len(values) / 6` to increase make sure that almost all values are
        # within the boundaries and we don't have to cut the index too often.
        mean = len(values) / 2
        stddev = len(values) / 6.5

        def get_value(index):
            # We need to consider how to deal with the values which lie outside of the boundaries.
            # Perhaps, regenerate such values?
            index = int(index)
            if index < 0:
                index = 0
            elif index >= len(values):
                index = len(values) - 1
            return values[index]

        return [get_value(n) for n in _rng.normal(loc=mean, scale=stddev, size=size)]

    @staticmethod
    def _noncentral_chisquare(size: int, values: Sequence[TVar], _: Sequence[float]):
        # Define `df` and `nonc` parameters in a way to minimize chances that generated values are
        # out of bounds of the `values` array.
        df = len(values) / 10
        nonc = len(values) / 3.5

        def get_value(index):
            # We need to consider how to deal with the values which lie outside of the boundaries.
            # Perhaps, regenerate such values?
            index = int(index)
            if index < 0:
                index = 0
            elif index >= len(values):
                index = len(values) - 1
            return values[index]

        return [get_value(n) for n in _rng.noncentral_chisquare(df=df, nonc=nonc, size=size)]

    @staticmethod
    def _uniform(size: int, values: Sequence[TVar], _: Sequence[float]):
        def get_value(index):
            index = int(index)
            return values[index]

        return [get_value(n) for n in _rng.uniform(low=0, high=len(values), size=size)]

    @staticmethod
    def _mixed(size: int, children: Sequence[RandomDistribution], probs: Sequence[float]):
        if probs is None:
            raise ValueError(f"probs must be specified for mixed distributions: {str(children)}")

        result = []
        for child_distr, prob in zip(children, probs):
            if not isinstance(child_distr, RandomDistribution):
                raise ValueError(
                    f"children must be of type RandomDistribution for mixed distribution, child_distr: {child_distr}"
                )
            child_size = int(size * prob)
            result.append(child_distr.generate(child_size))

        return list(chain.from_iterable(result))


_NO_DEFAULT = object()


@dataclass
class ArrayRandomDistribution(RandomDistribution):
    """Produces random array sequence of the specified values with the specified distribution."""

    lengths_distr: RandomDistribution = _NO_DEFAULT
    value_distr: RandomDistribution = _NO_DEFAULT

    def __init__(self, lengths_distr: RandomDistribution, value_distr: RandomDistribution):
        self.lengths_distr = lengths_distr
        self.value_distr = value_distr
        self.distribution_type = value_distr.distribution_type

    def __str__(self):
        distr_str = f"{super().__str__()}"
        distr_str += f"array_{str(self.value_distr)}_{str(self.lengths_distr)}"
        return distr_str

    def generate(self, size: int):
        """Generate random array sequence of the given size."""
        arrays = []
        lengths = self.lengths_distr.generate(size)

        for length in lengths:
            if not isinstance(length, int):
                raise ValueError("length must be an int for array generation")
            values = self.value_distr.generate(length)
            arrays.append(values)
        return arrays


@dataclass
class DocumentRandomDistribution(RandomDistribution):
    """Produces random document sequence of the specified values with the specified distribution."""

    number_of_fields_distr: RandomDistribution = _NO_DEFAULT
    fields_distr: RandomDistribution = _NO_DEFAULT
    field_to_distribution: dict = _NO_DEFAULT

    def __init__(
        self,
        number_of_fields_distr: RandomDistribution,
        fields_distr: RandomDistribution,
        field_to_distribution: dict,
    ):
        self.number_of_fields_distr = number_of_fields_distr
        self.fields_distr = fields_distr
        self.field_to_distribution = field_to_distribution
        self.distribution_type = fields_distr.distribution_type

        for field in self.get_fields():
            if field not in self.field_to_distribution:
                raise ValueError("Must provide a RandomDistribution for each field")

    def __str__(self):
        return f"{super().__str__()}"

    def generate(self, size: int):
        """Generate random document sequence of the given size."""
        docs = []
        nums = self.number_of_fields_distr.generate(size)
        field_to_values = {}

        # Pre-generate values for each field with corresponding distribution.
        # Note that not all values generated would be used because the number of fields of a document is randomly generated as well.
        for field in self.get_fields():
            field_to_values[field] = self.field_to_distribution[field].generate(size)

        idx = 0
        for idx, num in enumerate(nums):
            doc = {}
            if not isinstance(num, int):
                raise ValueError("the number of fields must be an int for document generation")

            field_names = self.fields_distr.generate(num)
            for field in field_names:
                doc[field] = field_to_values[field][idx]

            docs.append(doc)

        return docs

    def get_fields(self):
        """Return a list of field names used to generate a random document."""
        return self.fields_distr.get_values()


if __name__ == "__main__":
    from collections import Counter

    def print_distr(title, distr, size=10000):
        """Print distribution."""
        print(f"\n{title}: {str(distr)}\n")
        rs = distr.generate(size)
        has_arrays = any(isinstance(elem, list) for elem in rs)
        has_dict = any(isinstance(elem, dict) for elem in rs)

        if not has_arrays and not has_dict:
            counter = Counter(rs)
            for value in [*Counter(rs)]:
                count = counter[value]
                if isinstance(value, float):
                    print(f'{value:.2f}\t{count}\t{(count//10)*"*"}')
                else:
                    print(f'{value}\t{count}\t{(count//10)*"*"}')
        else:
            for elem in rs:
                print(elem)

    choice = RandomDistribution.choice(
        values=["pooh", "rabbit", "piglet", "Chris"], weights=[0.5, 0.1, 0.1, 0.3]
    )
    print_distr("Choice", choice, 1000)

    string_generator = RangeGenerator(
        data_type=DataType.STRING, interval_begin="hello_a", interval_end="hello__"
    )
    str_normal = RandomDistribution.normal(string_generator)
    print_distr("Normal for strings", str_normal)

    int_noncentral_chisquare = RandomDistribution.noncentral_chisquare(list(range(1, 30)))
    print_distr("Noncentral Chisquare for integers", int_noncentral_chisquare)

    float_uniform = RandomDistribution.uniform(RangeGenerator(DataType.DOUBLE, 0.1, 10.0, 0.37))
    print_distr("Uniform for floats", float_uniform)

    float_normal = RandomDistribution.normal(RangeGenerator(DataType.DOUBLE, 0.1, 10.0, 0.37))
    print_distr("Normal for floats", float_normal)

    FOUR_DAYS_IN_SECONDS = 60 * 20 * 24 * 12

    date_uniform = RandomDistribution.uniform(
        RangeGenerator(
            DataType.DATE, datetime(2007, 1, 1), datetime(2008, 1, 1), FOUR_DAYS_IN_SECONDS
        )
    )
    print_distr("Uniform for dates", date_uniform, size=1000)

    date_normal = RandomDistribution.normal(
        RangeGenerator(
            DataType.DATE, datetime(2007, 1, 1), datetime(2008, 1, 1), FOUR_DAYS_IN_SECONDS
        )
    )
    print_distr("Normal for dates", date_normal, size=1000)

    str_chisquare2 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "aa", "ba"))
    str_normal2 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "ap", "bp"))
    mixed = RandomDistribution.mixed(
        children=[float_uniform, str_chisquare2, str_normal2], weight=[0.3, 0.5, 0.2]
    )
    print_distr("Mixed", mixed, 20_000)

    int_normal = RandomDistribution.normal(RangeGenerator(DataType.INTEGER, 2, 10))

    arr_distr = ArrayRandomDistribution(int_normal, mixed)
    print_distr("Mixed Arrays", arr_distr, 100)

    mixed_with_arrays = RandomDistribution.mixed(
        children=[float_uniform, str_normal2, arr_distr], weight=[0.3, 0.2, 0.5]
    )
    nested_arr_distr = ArrayRandomDistribution(int_normal, mixed_with_arrays)

    print_distr("Mixed Nested Arrays", nested_arr_distr, 100)

    simple_doc_distr = DocumentRandomDistribution(
        RandomDistribution.normal(RangeGenerator(DataType.INTEGER, 1, 2)),
        RandomDistribution.uniform(["obj"]),
        {"obj": int_normal},
    )

    field_name_choice = RandomDistribution.uniform(["a", "b", "c", "d", "e", "f"])

    field_to_distr = {
        "a": int_normal,
        "b": str_normal,
        "c": mixed,
        "d": arr_distr,
        "e": nested_arr_distr,
        "f": simple_doc_distr,
    }
    nested_doc_distr = DocumentRandomDistribution(
        RandomDistribution.normal(RangeGenerator(DataType.INTEGER, 0, 7)),
        field_name_choice,
        field_to_distr,
    )

    print_distr("Nested Document generation", nested_doc_distr, 100)
