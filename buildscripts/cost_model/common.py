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
"""Impements commonly used types."""

import functools
import time

"""
Returns a preorder traversal (node, child 1...child n) of the QSN/execution tree 
For example, a query with an OR over 3 indices could turn into a tree rooted with a FETCH,
who has a single OR child, which in turn has 3 index scan children. 
This would return a preorder of [FETCH, OR, IXSCAN1, IXSCAN2, IXSCAN3].
"""


def preorder(node):
    res = [node]
    for child in node.children:
        res += preorder(child)
    return res


def timer_decorator(func):
    """Measure and print out execution time of the decorated function."""

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        t0 = time.perf_counter()
        result = func(*args, **kwargs)
        t1 = time.perf_counter()
        print(f"{func.__name__} took {t1-t0}s.")
        return result

    return wrapper
