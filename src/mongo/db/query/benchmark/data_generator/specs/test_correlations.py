import dataclasses

from datagen.distribution import correlation, uniform
from datagen.random import default_random, global_faker, numpy_random
from datagen.util import Specification


@dataclasses.dataclass
class Uncorrelated:
    field1: Specification(source=lambda: default_random().random())
    field2: Specification(source=correlation(lambda: default_random().random(), "a"))


@dataclasses.dataclass
class PartialCorrelation:
    field1: Specification(source=correlation(lambda: global_faker().pybool(), "a"))
    field2: Specification(
        source=uniform([correlation(lambda: global_faker().pybool(), "a"), uniform([1, 2])])
    )


@dataclasses.dataclass
class TwoCorrelations:
    g1field1: Specification(source=correlation(lambda: global_faker().pyint(max_value=10), "a"))
    g1field2: Specification(source=correlation(uniform([True, False]), "a"))
    g2field1: Specification(source=correlation(lambda: default_random().randint(0, 10), "b"))
    g2field2: Specification(
        source=correlation(lambda: global_faker().pybool(truth_probability=60), "b")
    )


@dataclasses.dataclass
class CorrelationNotSupportedForNumpy:
    field1: Specification(source=correlation(lambda: numpy_random().integers(0, 10, 1), "a"))
    field2: Specification(source=correlation(lambda: default_random().randint(0, 10), "a"))
