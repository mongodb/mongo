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

"""Things relevant to the RNG aspect of the data generator."""

from __future__ import annotations

import math
import random

__all__ = ["CorrelatedRng", "DataType", "RandomDistribution"]


####################################################################################################
#
# Correlated data generator definitions begin here.
#
####################################################################################################


class CorrelatedRng(random.Random):
    """A custom random number generator that makes it easier to generate correlated data."""

    def __init__(self, *args, **kwargs):
        super(CorrelatedRng, self).__init__(*args, **kwargs)
        self._cache = {}
        self._stack = []  # A stack of states.

    def _randbelow(self, n):
        """Override with multiplication-based generation instead of modulus to generate integers."""
        return math.floor(n * self.random())

    def clear(self):
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
