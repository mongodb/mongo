import dataclasses

from datagen.distribution import correlation
from datagen.random import *
from datagen.util import Specification


# Tests that external RNGs all use the seed.
@dataclasses.dataclass
class Seed:
    default_random: Specification(source=lambda: default_random().random())
    numpy_random: Specification(source=lambda: numpy_random().random())
    faker: Specification(source=lambda: global_faker().pyint())
    default_random_correlated: Specification(
        source=correlation(lambda: default_random().random(), "a")
    )
    faker_correlated: Specification(source=correlation(lambda: global_faker().pyint(), "a"))
