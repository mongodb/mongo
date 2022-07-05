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
"""Define ABT physical tree and parse it from query explain."""

from __future__ import annotations
from dataclasses import dataclass

__all__ = ['Node', 'build']


@dataclass
class Node:
    """Represent ABT node."""

    node_type: str
    plan_node_id: int
    children: list[Node]

    def print(self, level=0):
        """Pretty print of the ABT."""
        print(f'{"| "*level}nodeType: {self.node_type}, plaNodeId: {self.plan_node_id}')
        for child in self.children:
            child.print(level + 1)


def build(optimizer_plan: dict[str, any]) -> Node:
    """Build ABT from query explain."""
    return parse_optimizer_node(optimizer_plan)


def parse_optimizer_node(explain_node: dict[str, any]) -> Node:
    """Recursively parse ABT node from query explain's node."""
    children = get_children(explain_node)
    return Node(node_type=explain_node['nodeType'], plan_node_id=explain_node['properties'].get(
        'planNodeID', -1), children=children)


def get_children(explain_node: dict[str, any]) -> list[Node]:
    """Get children nodes of the ABT node."""
    children_refs = ['child', 'leftChild', 'rightChild']
    children = []
    for ref in children_refs:
        if ref in explain_node:
            children.append(parse_optimizer_node(explain_node[ref]))
    return children
