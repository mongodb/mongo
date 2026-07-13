# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Define SBE execution tree and parse it from query explain."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Optional

import bson.json_util as json

__all__ = ["Node", "build_execution_tree"]


@dataclass
class Node:
    """Represent SBE tree node."""

    stage: str
    plan_node_id: int
    total_execution_time: int
    seeks: Optional[int]
    n_returned: int
    n_processed: int
    children: list[Node]

    def get_execution_time(self):
        """Execution time of the SBE node without execution time of its children."""
        return self.total_execution_time - sum(n.total_execution_time for n in self.children)

    def print(self, level=0):
        """Pretty print of the SBE tree."""
        print(
            f'{"| "*level}{self.stage}, planNodeId: {self.plan_node_id}, totalExecutionTime: {self.total_execution_time:,}, seeks: {self.seeks}, nReturned: {self.n_returned}, nProcessed: {self.n_processed}'
        )
        for child in self.children:
            child.print(level + 1)


def build_execution_tree(execution_stats: dict[str, Any]) -> Node:
    """Build SBE execution tree from 'executionStats' field of query explain."""
    assert execution_stats["executionSuccess"]
    return process_stage(execution_stats["executionStages"])


def process_stage(stage: dict[str, Any]) -> Node:
    """Parse the given SBE stage."""
    processors = {
        "filter": process_filter,
        "cfilter": process_filter,
        "traverse": process_traverse,
        "project": process_inner_node,
        "limit": process_inner_node,
        "ixscan_generic": process_seek,
        "scan": process_seek,
        "coscan": process_leaf_node,
        "nlj": process_nlj,
        "hj": process_hash_join_node,
        "mj": process_hash_join_node,
        "seek": process_seek,
        "ixseek": process_seek,
        "limitskip": process_inner_node,
        "group": process_inner_node,
        "union": process_union_node,
        "unique": process_unique_node,
        "unwind": process_unwind_node,
        "branch": process_branch_node,
    }

    processor = processors.get(stage["stage"])
    if processor is None:
        print(json.dumps(stage, indent=4))
        raise ValueError(f"Unknown stage: {stage}")

    return processor(stage)


def process_filter(stage: dict[str, Any]) -> Node:
    """Process filter stage."""
    input_stage = process_stage(stage["inputStage"])
    return Node(**get_common_fields(stage), n_processed=stage["numTested"], children=[input_stage])


def process_traverse(stage: dict[str, Any]) -> Node:
    """Process traverse"""
    outer_stage = process_stage(stage["outerStage"])
    inner_stage = process_stage(stage["innerStage"])
    return Node(
        **get_common_fields(stage),
        n_processed=stage["nReturned"],
        children=[outer_stage, inner_stage],
    )


def process_hash_join_node(stage: dict[str, Any]) -> Node:
    """Process hj node."""
    outer_stage = process_stage(stage["outerStage"])
    inner_stage = process_stage(stage["innerStage"])
    n_processed = outer_stage.n_returned + inner_stage.n_returned
    return Node(
        **get_common_fields(stage), n_processed=n_processed, children=[outer_stage, inner_stage]
    )


def process_nlj(stage: dict[str, Any]) -> Node:
    """Process nlj stage."""
    outer_stage = process_stage(stage["outerStage"])
    inner_stage = process_stage(stage["innerStage"])
    n_processed = stage["totalDocsExamined"]
    return Node(
        **get_common_fields(stage), n_processed=n_processed, children=[outer_stage, inner_stage]
    )


def process_inner_node(stage: dict[str, Any]) -> Node:
    """Process SBE stage with one input stage."""
    input_stage = process_stage(stage["inputStage"])
    return Node(
        **get_common_fields(stage), n_processed=input_stage.n_returned, children=[input_stage]
    )


def process_leaf_node(stage: dict[str, Any]) -> Node:
    """Process SBE stage without input stages."""
    return Node(**get_common_fields(stage), n_processed=stage["nReturned"], children=[])


def process_seek(stage: dict[str, Any]) -> Node:
    """Process seek stage."""
    return Node(**get_common_fields(stage), n_processed=stage["numReads"], children=[])


def process_union_node(stage: dict[str, Any]) -> Node:
    """Process union stage."""
    children = [process_stage(child) for child in stage["inputStages"]]
    return Node(**get_common_fields(stage), n_processed=stage["nReturned"], children=children)


def process_unwind_node(stage: dict[str, Any]) -> Node:
    """Process unwind stage."""
    input_stage = process_stage(stage["inputStage"])
    return Node(
        **get_common_fields(stage), n_processed=input_stage.n_returned, children=[input_stage]
    )


def process_unique_node(stage: dict[str, Any]) -> Node:
    """Process unique stage."""
    input_stage = process_stage(stage["inputStage"])
    n_processed = stage["dupsTested"]
    return Node(**get_common_fields(stage), n_processed=n_processed, children=[input_stage])


def process_branch_node(stage: dict[str, Any]) -> Node:
    """Process unique stage."""
    then_stage = process_stage(stage["thenStage"])
    else_stage = process_stage(stage["elseStage"])
    n_processed = then_stage.n_returned + else_stage.n_returned
    return Node(
        **get_common_fields(stage), n_processed=n_processed, children=[then_stage, else_stage]
    )


def get_common_fields(json_stage: dict[str, Any]) -> dict[str, Any]:
    """Extract common field from json representation of SBE stage."""
    return {
        "stage": json_stage["stage"],
        "plan_node_id": json_stage["planNodeId"],
        "total_execution_time": json_stage["executionTimeNanos"],
        "n_returned": json_stage["nReturned"],
        "seeks": json_stage.get("seeks"),
    }
