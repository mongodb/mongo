# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Parse explain and extract parameters."""

from __future__ import annotations

from collections import defaultdict, deque
from typing import Any, Mapping, Sequence

import bson.json_util as json
import execution_tree_classic as classic
import query_solution_tree as qsn
from common import preorder
from config import QuerySolutionCalibrationConfig
from cost_estimator import CostModelParameters, ExecutionStats
from database_instance import DatabaseInstance
from workload_execution import QueryParameters


async def extract_parameters(
    config: QuerySolutionCalibrationConfig, database: DatabaseInstance, qsn_types: Sequence[str]
) -> Mapping[str, Sequence[CostModelParameters]]:
    """Read measurements from database and extract cost model input parameters for the given QSN types."""
    stats = defaultdict(list)

    docs = await database.get_all_documents(config.input_collection_name)
    for result in docs:
        explain = json.loads(result["explain"])
        query_parameters = QueryParameters.from_json(result["query_parameters"])
        res = parse_explain(explain, qsn_types)
        for qsn_type, es in res.items():
            stats[qsn_type] += [
                CostModelParameters(execution_stats=stat, query_params=query_parameters)
                for stat in es
            ]
        if config.trace and len(res) > 0:
            print(res)
    return stats


def parse_explain(
    explain: Mapping[str, Any], qsn_types: Sequence[str]
) -> Mapping[str, Sequence[ExecutionStats]]:
    try:
        et = classic.build_execution_tree(explain["executionStats"])
        qt = qsn.build(explain["queryPlanner"]["winningPlan"])
    except Exception as e:
        print(f"*** Failed to parse explain with the following error: {e}")
        print(explain)
        raise e
    return get_execution_stats(et, qt, qsn_types)


def get_execution_stats(
    et: classic.Node, qt: qsn.Node, qsn_types: Sequence[str]
) -> Mapping[str, Sequence[ExecutionStats]]:
    if len(qsn_types) == 0:
        qsn_types = get_qsn_types(qt)
    qt_preorder, et_preorder = preorder(qt), preorder(et)
    assert len(qt_preorder) == len(et_preorder)

    result: Mapping[str, ExecutionStats] = defaultdict(list)
    for qnode, enode in zip(qt_preorder, et_preorder):
        if qnode.node_type not in qsn_types:
            print(
                "Encountered unexpected node type during execution stats extraction: "
                + qnode.node_type
            )
            continue

        result[qnode.node_type].append(
            ExecutionStats(
                execution_time=enode.get_execution_time(),
                n_returned=enode.n_returned,
                n_processed=enode.n_processed,
                n_processed_per_child=[child.n_processed for child in enode.children],
                # This will be 0 in case there are no input stages
                n_children=len(enode.children),
                # Seeks will be None for any node but IXSCAN.
                seeks=enode.seeks,
                # n_index_fields will be None for any node but IXSCAN.
                n_index_fields=enode.n_index_fields,
                n_top_level_and_children=enode.n_top_level_and_children,
            )
        )
    return result


def get_qsn_types(pt: qsn.Node) -> Sequence[str]:
    """Extract types of all QS nodes in the given QSN tree"""
    qsn_types = set()
    queue: deque[qsn.Node] = deque()
    queue.append(pt)

    while len(queue) > 0:
        size = len(queue)
        for _ in range(size):
            node = queue.popleft()
            qsn_types.add(node.node_type)
            for child in node.children:
                queue.append(child)
    return qsn_types
