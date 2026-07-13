# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

"""
External RNGs

Instead of making one's own instance of an external RNG, one should use the RNGs defined in
this module because they can have two additional features.
* the RNG is on the same seed as the data generator.
* if possible, the RNG supports the correlation features of the data generator.

Extant external RNGs:
* `random`
* `faker`
* `numpy` (does not support correlation features)

Correlation in this data generator works by replacing calls to the python standard `random.Random`
with calls to `CorrelatedRng`, which is defined in this module. modified . Any external RNG you
would like to add to this module can be made to support correlation it uses `random.Random`
underneath, and you can change the pointer to `random.Random` to point to `CorrelatedRNG` instead.
"""

import math
import random

import faker
import numpy

GLOBAL_RANDOM = None
GLOBAL_NUMPY_RANDOM = None
GLOBAL_FAKER = None


def set_global_seed(seed) -> None:
    """Initializes all random generators on the same seed."""
    global GLOBAL_RANDOM
    global GLOBAL_NUMPY_RANDOM
    global GLOBAL_FAKER

    GLOBAL_RANDOM = CorrelatedRng(seed)
    GLOBAL_NUMPY_RANDOM = numpy.random.default_rng(int(seed))
    GLOBAL_FAKER = faker.Faker(generator=CorrelatedFakerGenerator(), use_weighting=True)


def default_random() -> random.Random:
    assert GLOBAL_RANDOM is not None
    return GLOBAL_RANDOM


def numpy_random() -> numpy.random.Generator:
    assert GLOBAL_NUMPY_RANDOM is not None
    if GLOBAL_RANDOM._stack:
        assert False, "Correlation is not supported in the numpy Generator."
    return GLOBAL_NUMPY_RANDOM


def global_faker() -> faker.Faker:
    assert GLOBAL_FAKER is not None
    return GLOBAL_FAKER


class CorrelatedRng(random.Random):
    """A custom random number generator that makes it easier to generate correlated data."""

    def __init__(self, seed, *args, **kwargs):
        super(CorrelatedRng, self).__init__(*args, **kwargs)
        self.seed(seed)
        self._cache = {}  # Saved correlation states.
        self._stack = []  # A stack of current correlation states.

    def _randbelow(self, n):
        """Override with multiplication-based generation instead of modulus to generate integers."""
        return math.floor(n * self.random())

    def clear(self):
        """
        Clears any correlation state.

        Call this in between creating new documents to prevent values of the same field in
        different documents from being correlated with each other, i.e. identical.
        """
        self._cache.clear()
        self._stack.clear()

    def random(self, *args, **kwargs):
        """Override the random() function of the base RNG.

        Also shadow the random calls with the default state as well. This way, if a correlation is
        added/removed, there should be minimal/no effect on subsequent random generations.
        """
        value = super(CorrelatedRng, self).random(*args, **kwargs)

        # Recall the RNG from the stack and overwrite the generated value.
        if self._stack:
            # Save the base state
            base_state = self.getstate()
            self.setstate(self._stack[-1])
            # Actually generate using the topmost stack state.
            value = super(CorrelatedRng, self).random(*args, **kwargs)
            self._stack[-1] = self.getstate()
            self.setstate(base_state)

        return value

    def recall(self, name):
        self._stack.append(self._cache.setdefault(name, self.getstate()))

    def reset(self):
        if self._stack:
            self._stack.pop()


# This class makes it easier to hook the correlated RNG with the Faker machinery.
class CorrelatedFakerGenerator(faker.generator.Generator):
    def __init__(self, *args, **kwargs):
        super(CorrelatedFakerGenerator, self).__init__(*args, **kwargs)
        self.random = default_random()
