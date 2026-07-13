# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Define Query Solution tree (aka QSN tree) and parse it from query explain."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

__all__ = ["Node", "build"]


@dataclass
class Node:
    """Represent Query Solution node."""

    node_type: str
    children: list[Node]

    def print(self, level=0):
        """Pretty print of the QSN tree."""
        print(f'{"| "*level}{self.node_type}')
        for child in self.children:
            child.print(level + 1)


def build(optimizer_plan: dict[str, Any]) -> Node:
    """Build QSN tree from query explain."""

    return parse_optimizer_node(optimizer_plan)


def parse_optimizer_node(explain_node: dict[str, Any]) -> Node:
    """Recursively parse QSN from query explain's node."""

    children = get_children(explain_node)
    return Node(node_type=explain_node["stage"], children=children)


def get_children(explain_node: dict[str, Any]) -> list[Node]:
    """Get children nodes of the QSN."""

    if "inputStage" in explain_node:
        children = [parse_optimizer_node(explain_node["inputStage"])]
    elif "inputStages" in explain_node:
        children = [parse_optimizer_node(child) for child in explain_node["inputStages"]]
    else:
        children = []
    return children


if __name__ == "__main__":
    import json

    explain = """
    {
    "explainVersion": "2",
    "queryPlanner": {
        "planCacheShapeHash": "5C2C3085",
        "planCacheKey": "9EB06642",
        "optimizationTimeMillis": 0,
        "maxIndexedOrSolutionsReached": false,
        "maxIndexedAndSolutionsReached": false,
        "maxScansToExplodeReached": false,
        "winningPlan": {
        "isCached": false,
        "queryPlan": {
            "stage": "FETCH",
            "planNodeId": 5,
            "inputStage": {
            "stage": "OR",
            "planNodeId": 4,
            "inputStages": [
                {
                "stage": "FETCH",
                "planNodeId": 2,
                "filter": {
                    "as": {
                    "$gt": 10
                    }
                },
                "inputStage": {
                    "stage": "IXSCAN",
                    "planNodeId": 1,
                    "keyPattern": {
                    "as": 1,
                    "mixed1": 1
                    },
                    "indexName": "as_1_mixed1_1",
                    "isMultiKey": true,
                    "multiKeyPaths": {
                    "as": [
                        "as"
                    ],
                    "mixed1": []
                    },
                    "isUnique": false,
                    "isSparse": false,
                    "isPartial": false,
                    "indexVersion": 2,
                    "direction": "forward",
                    "indexBounds": {
                    "as": [
                        "[-inf, 4)"
                    ],
                    "mixed1": [
                        "[MinKey, MaxKey]"
                    ]
                    }
                }
                },
                {
                "stage": "IXSCAN",
                "planNodeId": 3,
                "keyPattern": {
                    "as": 1,
                    "mixed1": 1
                },
                "indexName": "as_1_mixed1_1",
                "isMultiKey": true,
                "multiKeyPaths": {
                    "as": [
                    "as"
                    ],
                    "mixed1": []
                },
                "isUnique": false,
                "isSparse": false,
                "isPartial": false,
                "indexVersion": 2,
                "direction": "forward",
                "indexBounds": {
                    "as": [
                    "[4, 4]"
                    ],
                    "mixed1": [
                    "[MinKey, MaxKey]"
                    ]
                }
                }
            ]
            }
        }
        }
    }
    }
    """

    explain_json = json.loads(explain)
    qsn = build(explain_json["queryPlanner"]["winningPlan"]["queryPlan"])
    qsn.print()
