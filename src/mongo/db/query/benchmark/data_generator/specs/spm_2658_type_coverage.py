import dataclasses

from datagen.distribution import *
from datagen.random import default_random, global_faker
from datagen.util import MISSING, Specification


@dataclasses.dataclass
class InnerObjectInList:
    one_word_strings: Specification(lambda: global_faker().country())  # Single-word strings
    double: Specification(lambda: default_random().expovariate(1))  # Double
    int: Specification(choice(list(range(10)), list(range(10))))  # Int
    mwstr: Specification(lambda: global_faker().address())  # Multi-word strings
    mixed_scalars: Specification(
        choice(
            [
                choice([True, False], [6, 4]),
                lambda: global_faker().free_email_domain(),
                lambda: default_random().weibullvariate(1, 1.5),
            ],
            [1, 1, 1],
        )
    )  # Mixed scalars


@dataclasses.dataclass
class InnerObject:
    d: Specification(lambda: default_random().betavariate(2, 5))  # Double
    i: Specification(lambda: int(default_random().triangular(0, 100, 35)))  # Int
    mwstr: Specification(lambda: global_faker().sentence())  # Multi-word strings
    owstr_low_ndv: Specification(
        lambda: default_random().choice(["North", "East", "West", "South", "New", "Lake", "Port"])
    )  # Single-word strings w/ low ndv
    mixed: Specification(
        choice(
            [
                choice([True, False, None], [10, 7, 2]),
                lambda: global_faker().vin(),
                lambda: int(default_random().gauss(40000, 15000)),
            ],
            [3, 1, 2],
        )
    )  # Mixed scalars.


def pareto_func(alpha):
    return lambda: min(1000000, int(default_random().paretovariate(alpha)))


@dataclasses.dataclass
class TypeCoverage:
    object_list: Specification(
        array(lambda: min(45, int(default_random().expovariate(1)) + 1), InnerObjectInList)
    )
    single_object: Specification(InnerObject)
    multi_word_strings: Specification(lambda: global_faker().catch_phrase())  # Multi-word strings
    one_word_str_high_ndv: Specification(
        lambda: global_faker().city()
    )  # Single-word strings w/ high ndv
    double: Specification(
        choice([lambda: float(global_faker().latitude()), None], [7, 3])
    )  # Double with some nulls
    multikey_ints: Specification(
        choice(
            [
                pareto_func(0.5),
                array(uniform([3, 4, 5, 6]), pareto_func(0.5)),
            ],
            [1, 1],
        )
    )  # Ints + Array of ints
    lots_missing: Specification(
        choice([pareto_func(1), MISSING], [1, 95])
    )  # Int with very high proportion of missing values
    int_unindexed: Specification(normal(list(range(100))))  # unindexed field int
