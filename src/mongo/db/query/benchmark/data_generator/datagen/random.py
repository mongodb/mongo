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
