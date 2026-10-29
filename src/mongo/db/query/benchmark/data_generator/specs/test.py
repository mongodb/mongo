# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

"""
Example collection spec used for smoke tests of the generator itself.
"""

import dataclasses

import pymongo
from datagen.util import Specification


@dataclasses.dataclass
class TestGrandchild:
    g: Specification(int)


@dataclasses.dataclass
class TestChild:
    c: Specification(TestGrandchild)


@dataclasses.dataclass
class Test:
    i: Specification(int)
    child: Specification(TestChild)


def test_index() -> list[pymongo.IndexModel]:
    return [
        pymongo.IndexModel(keys="i", name="i_idx"),
    ]
