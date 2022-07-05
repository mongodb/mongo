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
"""Define SBE execution tree and parse it from query explain."""

from __future__ import annotations
from dataclasses import dataclass
import json

__all__ = ['Node', 'build_execution_tree']


@dataclass
class Node:
    """Represent SBE tree node."""

    stage: str
    plan_node_id: int
    total_execution_time: int
    n_returned: int
    n_processed: int
    children: list[Node]

    def get_execution_time(self):
        """Execution time of the SBE node without execuion time of its children."""
        return self.total_execution_time - sum(n.total_execution_time for n in self.children)

    def print(self, level=0):
        """Pretty print of the SBE tree."""
        print(
            f'{"| "*level}stage: {self.stage}, plaNodeId: {self.plan_node_id}, totalExecutionTime: {self.total_execution_time}, nReturned: {self.n_returned}, nProcessed: {self.n_processed}'
        )
        for child in self.children:
            child.print(level + 1)


def build_execution_tree(execution_stats: dict[str, any]) -> Node:
    """Build SBE executioon tree from 'executionStats' field of query explain."""
    assert execution_stats['executionSuccess']
    return process_stage(execution_stats['executionStages'])


def process_stage(stage: dict[str, any]) -> Node:
    """Parse the given SBE stage."""
    processors = {
        'filter': process_filter,
        'cfilter': process_filter,
        'traverse': process_inner_outer,
        'project': process_inner_node,
        'limit': process_inner_node,
        'scan': process_leaf_node,
        'coscan': process_leaf_node,
        'nlj': process_nlj,
        'seek': process_seek,
        'ixseek': process_seek,
        'limitskip': process_inner_node,
    }

    processor = processors.get(stage['stage'])
    if processor is None:
        print(json.dumps(stage, indent=4))
        raise ValueError(f'Unknown stage: {stage}')

    return processor(stage)


def process_filter(stage: dict[str, any]) -> Node:
    """Process filter stage."""
    input_stage = process_stage(stage['inputStage'])
    return Node(**get_common_fields(stage), n_processed=stage['numTested'], children=[input_stage])


def process_inner_outer(stage: dict[str, any]) -> Node:
    """Process SBE stage with two (inner and outer) input stages."""
    outer_stage = process_stage(stage['outerStage'])
    inner_stage = process_stage(stage['innerStage'])
    return Node(**get_common_fields(stage), n_processed=stage['nReturned'],
                children=[outer_stage, inner_stage])


def process_nlj(stage: dict[str, any]) -> Node:
    """Process nlj stage."""
    outer_stage = process_stage(stage['outerStage'])
    inner_stage = process_stage(stage['innerStage'])
    n_processed = stage['totalKeysExamined'] + stage['totalDocsExamined']
    return Node(**get_common_fields(stage), n_processed=n_processed,
                children=[outer_stage, inner_stage])


def process_inner_node(stage: dict[str, any]) -> Node:
    """Process SBE stage with one input stage."""
    input_stage = process_stage(stage['inputStage'])
    return Node(**get_common_fields(stage), n_processed=stage['nReturned'], children=[input_stage])


def process_leaf_node(stage: dict[str, any]) -> Node:
    """Process SBE stage without input stages."""
    return Node(**get_common_fields(stage), n_processed=stage['nReturned'], children=[])


def process_seek(stage: dict[str, any]) -> Node:
    """Process seek stage."""
    return Node(**get_common_fields(stage), n_processed=stage['numReads'], children=[])


def get_common_fields(json_stage: dict[str, any]) -> dict[str, any]:
    """Exctract common field from json representation of SBE stage."""
    return {
        'stage': json_stage['stage'], 'plan_node_id': json_stage['planNodeId'],
        'total_execution_time': json_stage['executionTimeMicros'],
        'n_returned': json_stage['nReturned']
    }
