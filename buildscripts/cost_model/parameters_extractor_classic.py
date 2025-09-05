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
#
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

    result: Mapping[str, ExecutionStats] = defaultdict(list)
    for qnode, enode in zip(preorder(qt), preorder(et), strict=True):
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
                # Seeks will be None for any node but IXSCAN.
                seeks=enode.seeks,
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
