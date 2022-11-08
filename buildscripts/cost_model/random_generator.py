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
from enum import Enum
from itertools import chain
from typing import Generic, Sequence, TypeVar
import numpy as np

__all__ = ['RangeGenerator', 'DataType', 'RandomDistribution']


class DataType(Enum):
    """Data type enum for data generators."""

    STRING = 0
    INTEGER = 1
    FLOAT = 2


TVar = TypeVar('TVar', str, int, float)


@dataclass
class RangeGenerator(Generic[TVar]):
    """Produces a sequence of non-random data for the given interval and step."""

    data_type: DataType
    interval_begin: TVar
    interval_end: TVar
    step: int = 1

    def generate(self) -> Sequence[TVar]:
        """Generate the range."""

        gen_range_dict = {
            DataType.STRING: ansi_range, DataType.INTEGER: range, DataType.FLOAT: np.arange
        }

        gen_range = gen_range_dict.get(self.data_type)
        if gen_range is None:
            raise ValueError(f'Unsupported data type: {self.data_type}')

        return list(gen_range(self.interval_begin, self.interval_end, self.step))


def ansi_range(begin: str, end: str, step: int = 1):
    """Produces a sequence of string from begin to end."""

    alphabet_size = 28
    non_alpha_char = '_'

    def ansi_to_int(data: str) -> int:
        res = 0
        for char in data.lower():
            res = res * alphabet_size
            if 'a' <= char <= 'z':
                res += ord(char) - ord('a') + 1
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
                char = chr(remainder + ord('a') - 1)
            result.append(char)
        result.reverse()
        return ''.join(result)

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
            yield f'{prefix}{int_to_ansi(number)}'


class DistributionType(Enum):
    """An enum of distributions supported by Random Data Generator."""

    CHOICE = 0
    NORMAL = 1
    NONCENTRAL_CHISQUARE = 2
    UNIFORM = 3
    MIXED = 4


_rng = np.random.default_rng()


@dataclass
class RandomDistribution:
    """Produces random sequence of the specified values with the specified distribution."""

    distribution_type: DistributionType
    values: Union[Sequence[TVar], RangeGenerator]
    weights: Union[Sequence[float], None]

    @staticmethod
    def choice(values: Sequence[TVar], weights: Union[Sequence[float], RangeGenerator]):
        """Create choice distribution."""
        return RandomDistribution(distribution_type=DistributionType.CHOICE, values=values,
                                  weights=weights)

    @staticmethod
    def normal(values: Union[Sequence[TVar], RangeGenerator]):
        """Create normal distribution."""
        return RandomDistribution(distribution_type=DistributionType.NORMAL, values=values,
                                  weights=None)

    @staticmethod
    def noncentral_chisquare(values: Union[Sequence[TVar], RangeGenerator]):
        """Create Non Central Chi2 distribution."""
        return RandomDistribution(distribution_type=DistributionType.NONCENTRAL_CHISQUARE,
                                  values=values, weights=None)

    @staticmethod
    def uniform(values: Union[Sequence[TVar], RangeGenerator]):
        """Create uniform distribution."""
        return RandomDistribution(distribution_type=DistributionType.UNIFORM, values=values,
                                  weights=None)

    @staticmethod
    def mixed(children: Sequence[RandomDistribution],
              weight: Union[Sequence[float], RangeGenerator]):
        """Create mixed distribution."""
        return RandomDistribution(distribution_type=DistributionType.MIXED, values=children,
                                  weights=weight)

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
            raise ValueError(f'values and probs must be the same size: {probs} !! {values}')

        if len(values) == 0:
            raise ValueError(f"Values cannot be empty: {self.values}")

        generators = {
            DistributionType.CHOICE: RandomDistribution._choice,
            DistributionType.NORMAL: RandomDistribution._normal,
            DistributionType.NONCENTRAL_CHISQUARE: RandomDistribution._noncentral_chisquare,
            DistributionType.UNIFORM: RandomDistribution._uniform,
            DistributionType.MIXED: RandomDistribution._mixed,
        }

        gen = generators.get(self.distribution_type)
        if gen is None:
            raise ValueError(f"Unsupported distribution type: {self.distribution_type}")

        return gen(size, values, probs)

    def get_values(self):
        """Return a list of values used to generate a random sequence."""
        if self.distribution_type == DistributionType.MIXED:
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
        # withing the boundaries and we don't have to cut the index too often.
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
            raise ValueError("props must be specified for mixed distribution")

        result = []
        for child_distr, prob in zip(children, probs):
            if not isinstance(child_distr, RandomDistribution):
                raise ValueError(
                    "children must be of type RandomDistribution for mixed distribution")
            child_size = int(size * prob)
            result.append(child_distr.generate(child_size))

        return list(chain.from_iterable(result))


@dataclass
class ArrayRandomDistribution(RandomDistribution):
    """Produces random array sequence of the specified values with the specified distribution."""

    lengths_distr: RandomDistribution
    value_distr: RandomDistribution

    def __init__(self, lengths_distr: RandomDistribution, value_distr: RandomDistribution):
        self.lengths_distr = lengths_distr
        self.value_distr = value_distr

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

    number_of_fields_distr: RandomDistribution
    fields_distr: RandomDistribution
    field_to_distribution: dict

    def __init__(self, number_of_fields_distr: RandomDistribution, fields_distr: RandomDistribution,
                 field_to_distribution: dict):
        self.number_of_fields_distr = number_of_fields_distr
        self.fields_distr = fields_distr
        self.field_to_distribution = field_to_distribution

        for field in self.get_fields():
            if field not in self.field_to_distribution:
                raise ValueError("Must provide a RandomDistribution for each field")

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


if __name__ == '__main__':
    from collections import Counter

    def print_distr(title, distr, size=10000):
        """Print distribution."""
        print(f'\n{title}\n')
        rs = distr.generate(size)
        has_arrays = any(isinstance(elem, list) for elem in rs)
        has_dict = any(isinstance(elem, dict) for elem in rs)

        if not has_arrays and not has_dict:
            counter = Counter(rs)
            for value in distr.get_values():
                count = counter[value]
                if isinstance(value, float):
                    print(f'{value:.2f}\t{count}\t{(count//10)*"*"}')
                else:
                    print(f'{value}\t{count}\t{(count//10)*"*"}')
        else:
            for elem in rs:
                print(elem)

    choice = RandomDistribution.choice(values=['pooh', 'rabbit', 'piglet', 'Chris'],
                                       weights=[0.5, 0.1, 0.1, 0.3])
    print_distr("Choice", choice, 1000)

    string_generator = RangeGenerator(data_type=DataType.STRING, interval_begin='hello_a',
                                      interval_end='hello__')
    str_normal = RandomDistribution.normal(string_generator)
    print_distr("Normal for strings", str_normal)

    int_noncentral_chisquare = RandomDistribution.noncentral_chisquare(list(range(1, 30)))
    print_distr("Noncentral Chisquare for integers", int_noncentral_chisquare)

    float_uniform = RandomDistribution.uniform(RangeGenerator(DataType.FLOAT, 0.1, 10.0, 0.37))
    print_distr("Uniform for floats", float_uniform)

    str_chisquare2 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "aa", "ba"))
    str_normal2 = RandomDistribution.normal(RangeGenerator(DataType.STRING, "ap", "bp"))
    mixed = RandomDistribution.mixed(children=[float_uniform, str_chisquare2, str_normal2],
                                     weight=[0.3, 0.5, 0.2])
    print_distr("Mixed", mixed, 20_000)

    int_normal = RandomDistribution.normal(RangeGenerator(DataType.INTEGER, 2, 10))

    arr_distr = ArrayRandomDistribution(int_normal, mixed)
    print_distr("Mixed Arrays", arr_distr, 100)

    mixed_with_arrays = RandomDistribution.mixed(children=[float_uniform, str_normal2, arr_distr],
                                                 weight=[0.3, 0.2, 0.5])
    nested_arr_distr = ArrayRandomDistribution(int_normal, mixed_with_arrays)

    print_distr("Mixed Nested Arrays", nested_arr_distr, 100)

    simple_doc_distr = DocumentRandomDistribution(
        RandomDistribution.normal(RangeGenerator(DataType.INTEGER, 1, 2)),
        RandomDistribution.uniform(["obj"]), {"obj": int_normal})

    field_name_choice = RandomDistribution.uniform(['a', 'b', 'c', 'd', 'e', 'f'])

    field_to_distr = {
        'a': int_normal, 'b': str_normal, 'c': mixed, 'd': arr_distr, 'e': nested_arr_distr,
        'f': simple_doc_distr
    }
    nested_doc_distr = DocumentRandomDistribution(
        RandomDistribution.normal(RangeGenerator(DataType.INTEGER, 0, 7)), field_name_choice,
        field_to_distr)

    print_distr("Nested Document generation", nested_doc_distr, 100)
