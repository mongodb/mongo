# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

"""Factories for distribution functions"""

from __future__ import annotations

import contextlib
import dataclasses
import typing

from datagen.random import default_random
from datagen.values import RangeGenerator

__all__ = [
    "array",
    "choice",
    "correlation",
    "distributionify",
    "normal",
    "uniform",
]


def correlation(distribution: callable, key: str) -> callable:
    """
    Correlates this `distribution` with all other distributions with the same `key`.

    Note that the correlation happens via sharing RNG state, so this function does not work if a
    distribution is a constant value.
    """
    distribution = distributionify(distribution)

    def result_func(**kwargs):
        with _CorrelatedContext(key):
            return distribution(**kwargs)

    return result_func


@dataclasses.dataclass
class _CorrelatedContext(contextlib.ContextDecorator):
    """This is a context that makes it easier to implement correlated data. For example,
    with _CorrelatedContext('a context key'):
        ...

    is equivalent to

    default_random().recall('a context key')
    ...
    default_random().reset()

    but with a lower likelihood of forgetting the `.reset()`.
    """

    name: str | None

    def __enter__(self):
        if self.name:
            default_random().recall(self.name)

    def __exit__(self, *_):
        if self.name:
            default_random().reset()


def array(lengths_distr: any, value_distr: any) -> callable:
    """
    Produces arrays, populating them using `value_distr`.

    Array lengths are decided by `lengths_distr`.
    """
    lengths_distr = distributionify(lengths_distr)
    value_distr = distributionify(value_distr)

    def result_func(**kwargs):
        length = lengths_distr(**kwargs)

        if not isinstance(length, int):
            raise ValueError("length must be an int for array generation")
        return [value_distr(**kwargs) for _ in range(length)]

    return result_func


def choice(
    values: list[any],
    weights: typing.Sequence[float] | RangeGenerator,
) -> callable:
    """
    Randomly chooses from values. Choices are weighted.
    """
    values = distributionify(values)
    if weights is None:
        raise ValueError("weights must be specified for choice distribution")

    if isinstance(weights, RangeGenerator):
        weights = weights.generate()

    if weights is not None:
        weights_sum = sum(weights)
        probs = [p / weights_sum for p in weights]
    else:
        probs = None

    def result_func(**kwargs):
        return default_random().choices(values, weights=probs, k=1)[0](**kwargs)

    return result_func


def uniform(values: list[any]) -> callable:
    """Randomly chooses between `values` with equal probability"""
    values = distributionify(values)

    def result_func(**kwargs):
        return default_random().choice(values)(**kwargs)

    return result_func


def normal(values: list[any]) -> callable:
    """
    Picks from `values` by generating positios based on the normal distribution.

    In other words, values are significantly more likely to be picked from the center of `values`
    than on the edges.
    """
    values = distributionify(values)

    def result_func(**kwargs):
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

        return get_value(default_random().normalvariate(mu=mean, sigma=stddev))(**kwargs)

    return result_func


def distributionify(input: any):
    if isinstance(input, RangeGenerator):
        input = input.generate()

    if isinstance(input, list):
        return [distributionify(i) for i in input]
    else:
        import datagen.util

        return datagen.util._distributionify(input)


_NO_DEFAULT = object()
