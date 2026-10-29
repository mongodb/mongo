# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

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
