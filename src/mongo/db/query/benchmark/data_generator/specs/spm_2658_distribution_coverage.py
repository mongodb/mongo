import dataclasses

from datagen.distribution import *
from datagen.random import default_random
from datagen.util import Specification


def randrange_func(start, stop, step=1):
    return lambda **kwargs: default_random().randrange(start, stop, step)


@dataclasses.dataclass
class Distribution:
    a: Specification(correlation(randrange_func(1, 5000), "a"))  # Field a: uniform
    b: Specification(lambda a: a, dependson=("a",))
    c: Specification(
        choice([lambda a: a, randrange_func(1, 5000)], [3, 1]), dependson=("a",)
    )  # Field c: 75% of values are correlated with column a, 25% are random chosen from uniform.
    d: Specification(
        choice([lambda a: a, randrange_func(1, 5000)], [1, 1]), dependson=("a",)
    )  # Field d: 50% of values are correlated with column a, 50% are randomly chosen from uniform.
    e: Specification(
        choice([lambda a: a, randrange_func(1, 5000)], [1, 3]), dependson=("a",)
    )  # Field e: 25% of values are correlated with column a, 75% are random chosen from uniform.
    f: Specification(
        randrange_func(1, 5000)
    )  # Field f: uniform distribution independent of column a.
    g: Specification(
        correlation(normal(list(range(5000))), "a")
    )  # Field g: 100% of values are correlated with column a, but the values are from a different distribution
    h: Specification(
        choice(
            [
                correlation(lambda: int(default_random().normalvariate(32342, 1232)), "a"),
                uniform([2, 3, 5, 8, 13, 21, 34, 55, 89]),
            ],
            [3, 1],
        )
    )  # Field h: 75% of values are correlated with column a and in a non-overlapping range compared to the range
    # of field a. 25% are randomly chosen from a discrete distribution with varying step sizes
    # between the elements.
    i: Specification(
        choice(
            [
                correlation(lambda: min(10000000, int(default_random().paretovariate(0.3))), "a"),
                randrange_func(2, 10000, 3),
            ],
            [1, 1],
        )
    )  # Field i: 50% of values are correlated with column a, 50% are randomly chosen from some other distribution.
    j: Specification(
        choice(
            [
                correlation(lambda: int(default_random().gammavariate(10, 2)), "a"),
                lambda: int(default_random().paretovariate(0.5)),
            ],
            [1, 3],
        )
    )  # Field j: 25% of values are correlated with column a, 75% are randomly chosen from some other distribution.
    k: Specification(
        lambda: int(default_random().triangular(1, 5000, 2345))
    )  # Field k: some other distribution independent of column a.
