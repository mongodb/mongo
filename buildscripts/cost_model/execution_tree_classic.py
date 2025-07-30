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

    def get_execution_time(self):
        """Execution time of this node without execution time of its children"""
        return self.execution_time_nanoseconds - sum(
            n.execution_time_nanoseconds for n in self.children
        )

    def print(self, level=0):
        """Pretty print the execution tree"""
        print(
            f'{"| " * level}{self.stage}, totalExecutionTime: {self.execution_time_nanoseconds:,}ns, seeks: {self.seeks}, nReturned: {self.n_returned}, nProcessed: {self.n_processed}'
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
        "MERGE_SORT": process_mergesort,
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
    return Node(**get_common_fields(stage), n_processed=stage["nReturned"], children=children)


def process_intersection(stage: dict[str, Any]) -> Node:
    children = [process_stage(child) for child in stage["inputStages"]]
    n_processed = sum(child.n_processed for child in children)
    return Node(**get_common_fields(stage), n_processed=n_processed, children=children)


def process_mergesort(stage: dict[str, Any]) -> Node:
    children = [process_stage(child) for child in stage["inputStages"]]
    return Node(**get_common_fields(stage), n_processed=stage["nReturned"], children=children)


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
    }
