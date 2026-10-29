# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Optional

import bson.json_util as json


@dataclass
class Node:
    """Represent Classic Execution Node"""

    stage: str
    execution_time_nanoseconds: int
    n_returned: int
    n_processed: int
    seeks: Optional[int]
    children: list[Node]
    n_index_fields: Optional[int]
    n_top_level_and_children: Optional[int]

    def get_execution_time(self):
        """Execution time of this node without execution time of its children"""
        return self.execution_time_nanoseconds - sum(
            n.execution_time_nanoseconds for n in self.children
        )

    def print(self, level=0):
        """Pretty print the execution tree"""
        print(
            f'{"| " * level}{self.stage}, totalExecutionTime: {self.execution_time_nanoseconds:,}ns, seeks: {self.seeks}, nReturned: {self.n_returned}, nProcessed: {self.n_processed}, nIndexFields: {self.n_index_fields}, nTopLevelAndChildren: {self.n_top_level_and_children}'
        )
        for child in self.children:
            child.print(level + 1)


def build_execution_tree(execution_stats: dict[str, Any]) -> Node:
    """Build Classic execution tree from 'executionStats' field of query explain"""
    assert execution_stats["executionSuccess"]
    return process_stage(execution_stats["executionStages"])


def process_stage(stage: dict[str, Any]) -> Node:
    """Parse the given execution stage"""
    processors = {
        "SUBPLAN": process_passthrough,
        "COLLSCAN": process_collscan,
        "IXSCAN": process_ixscan,
        "FETCH": process_fetch,
        "AND_HASH": process_intersection,
        "AND_SORTED": process_intersection,
        "OR": process_or,
        "SORT_MERGE": process_mergesort,
        "SORT": process_sort,
        "LIMIT": process_passthrough,
        "SKIP": process_skip,
        "PROJECTION_SIMPLE": process_passthrough,
        "PROJECTION_COVERED": process_passthrough,
        "PROJECTION_DEFAULT": process_passthrough,
    }
    processor = processors.get(stage["stage"])
    if processor is None:
        print(json.dumps(stage, indent=4))
        raise ValueError(f"Unknown stage: {stage}")

    return processor(stage)


def process_passthrough(stage: dict[str, Any]) -> Node:
    """Parse internal (non-leaf) execution stages with a single child, which process exactly the documents that they return."""
    input_stage = process_stage(stage["inputStage"])
    return Node(**get_common_fields(stage), n_processed=stage["nReturned"], children=[input_stage])


def process_collscan(stage: dict[str, Any]) -> Node:
    return Node(**get_common_fields(stage), n_processed=stage["docsExamined"], children=[])


def process_ixscan(stage: dict[str, Any]) -> Node:
    return Node(**get_common_fields(stage), n_processed=stage["keysExamined"], children=[])


def process_sort(stage: dict[str, Any]) -> Node:
    input_stage = process_stage(stage["inputStage"])
    return Node(
        **get_common_fields(stage), n_processed=input_stage.n_returned, children=[input_stage]
    )


def process_fetch(stage: dict[str, Any]) -> Node:
    input_stage = process_stage(stage["inputStage"])
    return Node(
        **get_common_fields(stage), n_processed=stage["docsExamined"], children=[input_stage]
    )


def process_or(stage: dict[str, Any]) -> Node:
    children = [process_stage(child) for child in stage["inputStages"]]
    # The number of processed documents is not just `stage["nReturned"]`, because that does
    # not include the potential duplicate documents which may had to be processed and dropped.
    return Node(
        **get_common_fields(stage),
        n_processed=sum(child.n_returned for child in children),
        children=children,
    )


def process_intersection(stage: dict[str, Any]) -> Node:
    children = [process_stage(child) for child in stage["inputStages"]]
    return Node(
        **get_common_fields(stage),
        n_processed=sum(child.n_returned for child in children),
        children=children,
    )


def process_mergesort(stage: dict[str, Any]) -> Node:
    children = [process_stage(child) for child in stage["inputStages"]]
    # The number of processed documents is not just `stage["nReturned"]`, because that does
    # not include the potential duplicate documents which may had to be processed and dropped.
    return Node(
        **get_common_fields(stage),
        n_processed=sum(child.n_returned for child in children),
        children=children,
    )


def process_skip(stage: dict[str, Any]) -> Node:
    input_stage = process_stage(stage["inputStage"])
    # This is different than the limit processor since the skip node processes both the documents it skips and the ones it passes up.
    return Node(
        **get_common_fields(stage), n_processed=input_stage.n_returned, children=[input_stage]
    )


def get_common_fields(json_stage: dict[str, Any]) -> dict[str, Any]:
    """Extract common fields from classic nodes"""
    return {
        "stage": json_stage["stage"],
        "execution_time_nanoseconds": json_stage["executionTimeNanos"],
        "n_returned": json_stage["nReturned"],
        "seeks": json_stage.get("seeks"),
        "n_index_fields": len(json_stage.get("keyPattern")) if "keyPattern" in json_stage else None,
        "n_top_level_and_children": (
            len(json_stage.get("filter").get("$and")) if "$and" in json_stage.get("filter") else 1
        )
        if "filter" in json_stage
        else None,
    }
