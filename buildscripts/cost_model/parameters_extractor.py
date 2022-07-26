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
"""Parse explain and extract parameters."""

from __future__ import annotations
from collections import deque, defaultdict
import json
from typing import Mapping, Sequence, TypeVar, Callable
from workload_execution import QueryParameters
from config import AbtCalibratorConfig
from database_instance import DatabaseInstance
from cost_estimator import ModelParameters, ExecutionStats
import execution_tree
import physical_tree

__all__ = ['extract_parameters']


def extract_parameters(config: AbtCalibratorConfig, database: DatabaseInstance,
                       abt_types: Sequence[str]) -> Mapping[str, Sequence[ModelParameters]]:
    """Read measurements from database and extract cost model parameters for the given ABT types."""

    stats = defaultdict(list)

    for result in database.get_all_documents(config.input_collection_name):
        explain = json.loads(result['explain'])
        query_parameters = QueryParameters.from_json(result['query_parameters'])
        res = parse_explain(explain, abt_types)
        for abt_type, stat in res.items():
            stats[abt_type].append(
                ModelParameters(execution_stats=stat, query_params=query_parameters))
        if config.trace and len(res) > 0:
            print(res)
    return stats


Node = TypeVar('Node')


def find_abt_node_by_type(root: physical_tree.Node, abt_type: str) -> physical_tree.Node | Node:
    """Find ABT node by its type."""
    abt_nodes = find_nodes(root, lambda node: node.node_type == abt_type)
    if len(abt_nodes) > 0:
        assert len(abt_nodes) == 1
        return abt_nodes[0]
    return None


def find_nodes(root: Node, predicate: Callable[[Node], bool]) -> list[Node]:
    """Find nodes in the given tree which satisfy the predicate."""

    def impl(node: Node, predicate: Callable[[Node], bool], result: list[Node]) -> Node:
        if predicate(node):
            result.append(node)
        for child in node.children:
            impl(child, predicate, result)

    result: list[Node] = []
    impl(root, predicate, result)
    return result


def get_excution_stats(root: execution_tree.Node, node_id: int) -> ExecutionStats:
    """Extract execution stats from the given Execution Tree for the ABT node defined with the given node_id."""
    queue: deque[execution_tree.Node] = deque()
    queue.append(root)

    execution_time: int = 0
    n_returned: int = root.n_returned
    n_processed: int = 0

    while len(queue) > 0:
        size = len(queue)
        for _ in range(size):
            node = queue.popleft()
            if node.plan_node_id == node_id:
                execution_time += node.get_execution_time()
                n_processed = max(n_processed, node.n_processed)
            for child in node.children:
                queue.append(child)

    return ExecutionStats(execution_time=execution_time, n_returned=n_returned,
                          n_processed=n_processed)


def parse_explain(explain: Mapping[str, any], abt_types: Sequence[str]):
    """Extract ExecutionStats from the given explain for the given ABT types."""

    result: Mapping[str, ExecutionStats] = {}
    et = execution_tree.build_execution_tree(explain['executionStats'])
    pt = physical_tree.build(explain['queryPlanner']['winningPlan']['optimizerPlan'])

    if len(abt_types) == 0:
        abt_types = get_abt_types(pt)

    for abt_type in abt_types:
        abt_node = find_abt_node_by_type(pt, abt_type)
        if abt_node is not None:
            execution_stats = get_excution_stats(et, abt_node.plan_node_id)
            result[abt_type] = execution_stats
    return result


def get_abt_types(pt: physical_tree.Node) -> Sequence[str]:
    """Extract types of all ABT nodes in the given ABT."""
    abt_types = set()
    queue: deque[physical_tree.Node] = deque()
    queue.append(pt)

    while len(queue) > 0:
        size = len(queue)
        for _ in range(size):
            node = queue.popleft()
            abt_types.add(node.node_type)
            for child in node.children:
                queue.append(child)
    return abt_types
