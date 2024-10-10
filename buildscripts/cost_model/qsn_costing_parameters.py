# Copyright (C) 2024-present MongoDB, Inc.
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
"""Prepare parameters for QSN cost calibration."""

from typing import Callable, Optional, TypeVar

import execution_tree as sbe
import pandas as pd
import query_solution_tree as qsn
from workload_execution import QueryParameters

Node = TypeVar("Node")


def parse_explain(explain: dict[str, any]) -> (qsn.Node, sbe.Node):
    qsn_tree = qsn.build(explain["queryPlanner"]["winningPlan"]["queryPlan"])
    sbe_tree = sbe.build_execution_tree(explain["executionStats"])
    return (qsn_tree, sbe_tree)


def find_first_node(root: Node, predicate: Callable[[Node], bool]) -> Optional[Node]:
    """Find the first node in the given tree which satisfy the predicate."""
    if predicate(root):
        return root
    else:
        for child in root.children:
            node = find_first_node(child, predicate)
            if node is not None:
                return node
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


class ParametersBuilder:
    """Prepare data for calibration from explain outputs."""

    def __init__(self):
        self.processors = {}
        self.default_processor = self._process_generic
        self.rows = []

    def process(self, explain: dict[str, any], params: QueryParameters):
        qsn_tree, sbe_tree = parse_explain(explain)
        self._process(qsn_tree, sbe_tree, params)

    def buildDataFrame(self) -> pd.DataFrame:
        return pd.DataFrame(
            self.rows,
            columns=[
                "stage",
                "execution_time",
                "n_processed",
                "seeks",
                "note",
                "keys_length_in_bytes",
                "average_document_size_in_bytes",
                "number_of_fields",
            ],
        )

    def _process(self, qsn_node: qsn.Node, sbe_tree: sbe.Node, params: QueryParameters):
        processor = self._get_processor(qsn_node.node_type)
        self.rows.append(processor(qsn_node.node_type, qsn_node.plan_node_id, sbe_tree, params))
        for child in qsn_node.children:
            self._process(child, sbe_tree, params)

    def _get_processor(self, stage: str):
        return self.processors.get(stage, self.default_processor)

    def _process_generic(
        self, stage: str, node_id: int, sbe_tree: sbe.Node, params: QueryParameters
    ):
        nodes: list[sbe.Node] = find_nodes(sbe_tree, lambda node: node.plan_node_id == node_id)
        if len(nodes) == 0:
            raise ValueError(f"Cannot find sbe nodes of {stage}")
        return ParametersBuilder._build_row(
            stage,
            params,
            execution_time=sum([node.get_execution_time() for node in nodes]),
            n_processed=max([node.n_processed for node in nodes]),
            seeks=sum([node.seeks for node in nodes if node.seeks]),
        )

    @staticmethod
    def _build_row(
        stage: str,
        params: QueryParameters,
        execution_time: int = None,
        n_processed: int = None,
        seeks: int = None,
    ):
        return [
            stage,
            execution_time,
            n_processed,
            seeks,
            params.note,
            params.keys_length_in_bytes,
            params.average_document_size_in_bytes,
            params.number_of_fields,
        ]


if __name__ == "__main__":
    import json

    explain = """
    {
    "explainVersion": "2",
    "queryPlanner": {
        "namespace": "calibration.index_scan_10000",
        "indexFilterSet": false,
        "parsedQuery": {
        "$or": [
            {
            "$and": [
                {
                "as": {
                    "$lt": 4
                }
                },
                {
                "as": {
                    "$gt": 10
                }
                }
            ]
            },
            {
            "as": {
                "$gt": 4
            }
            }
        ]
        },
        "planCacheShapeHash": "971E822A",
        "planCacheKey": "AA772AA3",
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
                        "[-inf.0, 4)"
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
                    "(4, inf.0]"
                    ],
                    "mixed1": [
                    "[MinKey, MaxKey]"
                    ]
                }
                }
            ]
            }
        }
        },
        "rejectedPlans": []
    },
    "executionStats": {
        "executionSuccess": true,
        "nReturned": 10000,
        "executionTimeMillis": 48,
        "totalKeysExamined": 49427,
        "totalDocsExamined": 10030,
        "executionStages": {
        "stage": "nlj",
        "planNodeId": 5,
        "nReturned": 10000,
        "executionTimeMillisEstimate": 46,
        "executionTimeMicros": 46370,
        "executionTimeNanos": 46370217,
        "opens": 1,
        "closes": 1,
        "saveState": 49,
        "restoreState": 49,
        "isEOF": 1,
        "totalDocsExamined": 10030,
        "totalKeysExamined": 49427,
        "collectionScans": 0,
        "collectionSeeks": 10030,
        "indexScans": 0,
        "indexSeeks": 2,
        "indexesUsed": [
            "as_1_mixed1_1",
            "as_1_mixed1_1"
        ],
        "innerOpens": 10000,
        "innerCloses": 1,
        "outerProjects": [],
        "outerCorrelated": [
            {
            "low": 21,
            "high": 0,
            "unsigned": false
            },
            {
            "low": 22,
            "high": 0,
            "unsigned": false
            },
            {
            "low": 18,
            "high": 0,
            "unsigned": false
            },
            {
            "low": 19,
            "high": 0,
            "unsigned": false
            },
            {
            "low": 20,
            "high": 0,
            "unsigned": false
            }
        ],
        "outerStage": {
            "stage": "unique",
            "planNodeId": 4,
            "nReturned": 10000,
            "executionTimeMillisEstimate": 33,
            "executionTimeMicros": 33589,
            "executionTimeNanos": 33589954,
            "opens": 1,
            "closes": 1,
            "saveState": 49,
            "restoreState": 49,
            "isEOF": 1,
            "dupsTested": 10030,
            "dupsDropped": 30,
            "keySlots": [
            {
                "low": 21,
                "high": 0,
                "unsigned": false
            }
            ],
            "inputStage": {
            "stage": "union",
            "planNodeId": 4,
            "nReturned": 10030,
            "executionTimeMillisEstimate": 30,
            "executionTimeMicros": 30777,
            "executionTimeNanos": 30777272,
            "opens": 1,
            "closes": 1,
            "saveState": 49,
            "restoreState": 49,
            "isEOF": 1,
            "inputSlots": [
                {
                "low": 5,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 6,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 7,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 9,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 4,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 16,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 17,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 7,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 14,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 15,
                "high": 0,
                "unsigned": false
                }
            ],
            "outputSlots": [
                {
                "low": 18,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 19,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 20,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 21,
                "high": 0,
                "unsigned": false
                },
                {
                "low": 22,
                "high": 0,
                "unsigned": false
                }
            ],
            "inputStages": [
                {
                "stage": "filter",
                "planNodeId": 2,
                "nReturned": 30,
                "executionTimeMillisEstimate": 0,
                "executionTimeMicros": 99,
                "executionTimeNanos": 99405,
                "opens": 1,
                "closes": 1,
                "saveState": 49,
                "restoreState": 49,
                "isEOF": 1,
                "numTested": 30,
                "filter": "traverseF(s10, lambda(l101.0) { ((move(l101.0) > s11) ?: false) }, false) ",
                "inputStage": {
                    "stage": "nlj",
                    "planNodeId": 2,
                    "nReturned": 30,
                    "executionTimeMillisEstimate": 0,
                    "executionTimeMicros": 93,
                    "executionTimeNanos": 93866,
                    "opens": 1,
                    "closes": 1,
                    "saveState": 49,
                    "restoreState": 49,
                    "isEOF": 1,
                    "totalDocsExamined": 30,
                    "totalKeysExamined": 30,
                    "collectionScans": 0,
                    "collectionSeeks": 30,
                    "indexScans": 0,
                    "indexSeeks": 1,
                    "indexesUsed": [
                    "as_1_mixed1_1"
                    ],
                    "innerOpens": 30,
                    "innerCloses": 1,
                    "outerProjects": [
                    {
                        "low": 4,
                        "high": 0,
                        "unsigned": false
                    },
                    {
                        "low": 5,
                        "high": 0,
                        "unsigned": false
                    },
                    {
                        "low": 6,
                        "high": 0,
                        "unsigned": false
                    }
                    ],
                    "outerCorrelated": [
                    {
                        "low": 3,
                        "high": 0,
                        "unsigned": false
                    },
                    {
                        "low": 4,
                        "high": 0,
                        "unsigned": false
                    },
                    {
                        "low": 5,
                        "high": 0,
                        "unsigned": false
                    },
                    {
                        "low": 6,
                        "high": 0,
                        "unsigned": false
                    },
                    {
                        "low": 7,
                        "high": 0,
                        "unsigned": false
                    }
                    ],
                    "outerStage": {
                    "stage": "unique",
                    "planNodeId": 1,
                    "nReturned": 30,
                    "executionTimeMillisEstimate": 0,
                    "executionTimeMicros": 32,
                    "executionTimeNanos": 32748,
                    "opens": 1,
                    "closes": 1,
                    "saveState": 49,
                    "restoreState": 49,
                    "isEOF": 1,
                    "dupsTested": 30,
                    "dupsDropped": 0,
                    "keySlots": [
                        {
                        "low": 3,
                        "high": 0,
                        "unsigned": false
                        }
                    ],
                    "inputStage": {
                        "stage": "cfilter",
                        "planNodeId": 1,
                        "nReturned": 30,
                        "executionTimeMillisEstimate": 0,
                        "executionTimeMicros": 24,
                        "executionTimeNanos": 24336,
                        "opens": 1,
                        "closes": 1,
                        "saveState": 49,
                        "restoreState": 49,
                        "isEOF": 1,
                        "numTested": 1,
                        "filter": "(exists(s1) && exists(s2)) ",
                        "inputStage": {
                        "stage": "ixseek",
                        "planNodeId": 1,
                        "nReturned": 30,
                        "executionTimeMillisEstimate": 0,
                        "executionTimeMicros": 20,
                        "executionTimeNanos": 20902,
                        "opens": 1,
                        "closes": 1,
                        "saveState": 49,
                        "restoreState": 49,
                        "isEOF": 1,
                        "indexName": "as_1_mixed1_1",
                        "keysExamined": 30,
                        "seeks": 1,
                        "numReads": 31,
                        "indexKeySlot": 6,
                        "recordIdSlot": 3,
                        "snapshotIdSlot": 4,
                        "indexIdentSlot": 5,
                        "outputSlots": [],
                        "indexKeysToInclude": "00000000000000000000000000000000",
                        "seekKeyLow": "s1 ",
                        "seekKeyHigh": "s2 "
                        }
                    }
                    },
                    "innerStage": {
                    "stage": "limit",
                    "planNodeId": 2,
                    "nReturned": 30,
                    "executionTimeMillisEstimate": 0,
                    "executionTimeMicros": 54,
                    "executionTimeNanos": 54643,
                    "opens": 30,
                    "closes": 1,
                    "saveState": 49,
                    "restoreState": 49,
                    "isEOF": 1,
                    "limit": 1,
                    "inputStage": {
                        "stage": "seek",
                        "planNodeId": 2,
                        "nReturned": 30,
                        "executionTimeMillisEstimate": 0,
                        "executionTimeMicros": 48,
                        "executionTimeNanos": 48162,
                        "opens": 30,
                        "closes": 1,
                        "saveState": 49,
                        "restoreState": 49,
                        "isEOF": 0,
                        "numReads": 30,
                        "recordSlot": 8,
                        "recordIdSlot": 9,
                        "seekRecordIdSlot": 3,
                        "snapshotIdSlot": 4,
                        "indexIdentSlot": 5,
                        "indexKeySlot": 6,
                        "indexKeyPatternSlot": 7,
                        "scanFieldNames": [
                        "as"
                        ],
                        "scanFieldSlots": [
                        {
                            "low": 10,
                            "high": 0,
                            "unsigned": false
                        }
                        ]
                    }
                    }
                }
                },
                {
                "stage": "unique",
                "planNodeId": 3,
                "nReturned": 10000,
                "executionTimeMillisEstimate": 29,
                "executionTimeMicros": 29986,
                "executionTimeNanos": 29986183,
                "opens": 1,
                "closes": 1,
                "saveState": 49,
                "restoreState": 49,
                "isEOF": 1,
                "dupsTested": 49397,
                "dupsDropped": 39397,
                "keySlots": [
                    {
                    "low": 14,
                    "high": 0,
                    "unsigned": false
                    }
                ],
                "inputStage": {
                    "stage": "cfilter",
                    "planNodeId": 3,
                    "nReturned": 49397,
                    "executionTimeMillisEstimate": 22,
                    "executionTimeMicros": 22086,
                    "executionTimeNanos": 22086929,
                    "opens": 1,
                    "closes": 1,
                    "saveState": 49,
                    "restoreState": 49,
                    "isEOF": 1,
                    "numTested": 1,
                    "filter": "(exists(s12) && exists(s13)) ",
                    "inputStage": {
                    "stage": "ixseek",
                    "planNodeId": 3,
                    "nReturned": 49397,
                    "executionTimeMillisEstimate": 18,
                    "executionTimeMicros": 18451,
                    "executionTimeNanos": 18451235,
                    "opens": 1,
                    "closes": 1,
                    "saveState": 49,
                    "restoreState": 49,
                    "isEOF": 1,
                    "indexName": "as_1_mixed1_1",
                    "keysExamined": 49397,
                    "seeks": 1,
                    "numReads": 49398,
                    "indexKeySlot": 17,
                    "recordIdSlot": 14,
                    "snapshotIdSlot": 15,
                    "indexIdentSlot": 16,
                    "outputSlots": [],
                    "indexKeysToInclude": "00000000000000000000000000000000",
                    "seekKeyLow": "s12 ",
                    "seekKeyHigh": "s13 "
                    }
                }
                }
            ]
            }
        },
        "innerStage": {
            "stage": "limit",
            "planNodeId": 5,
            "nReturned": 10000,
            "executionTimeMillisEstimate": 10,
            "executionTimeMicros": 10779,
            "executionTimeNanos": 10779434,
            "opens": 10000,
            "closes": 1,
            "saveState": 49,
            "restoreState": 49,
            "isEOF": 1,
            "limit": 1,
            "inputStage": {
            "stage": "seek",
            "planNodeId": 5,
            "nReturned": 10000,
            "executionTimeMillisEstimate": 8,
            "executionTimeMicros": 8835,
            "executionTimeNanos": 8835311,
            "opens": 10000,
            "closes": 1,
            "saveState": 49,
            "restoreState": 49,
            "isEOF": 0,
            "numReads": 10000,
            "recordSlot": 23,
            "recordIdSlot": 24,
            "seekRecordIdSlot": 21,
            "snapshotIdSlot": 22,
            "indexIdentSlot": 18,
            "indexKeySlot": 19,
            "indexKeyPatternSlot": 20,
            "scanFieldNames": [],
            "scanFieldSlots": []
            }
        }
        },
        "allPlansExecution": []
    },
    "command": {
        "find": "index_scan_10000",
        "filter": {
        "$or": [
            {
            "as": {
                "$gt": 10,
                "$lt": 4
            }
            },
            {
            "as": {
                "$gt": 4
            }
            }
        ]
        },
        "$db": "calibration"
    },
    "serverInfo": {
        "host": "ip-10-122-6-29",
        "port": 27017,
        "version": "8.0.0-alpha",
        "gitVersion": "unknown"
    },
    "serverParameters": {
        "internalQueryFacetBufferSizeBytes": 104857600,
        "internalQueryFacetMaxOutputDocSizeBytes": 104857600,
        "internalLookupStageIntermediateDocumentMaxSizeBytes": 104857600,
        "internalDocumentSourceGroupMaxMemoryBytes": 104857600,
        "internalQueryMaxBlockingSortMemoryUsageBytes": 104857600,
        "internalQueryProhibitBlockingMergeOnMongoS": 0,
        "internalQueryMaxAddToSetBytes": 104857600,
        "internalDocumentSourceSetWindowFieldsMaxMemoryBytes": 104857600,
        "internalQueryFrameworkControl": "trySbeEngine"
    },
    "ok": 1
    }
    """

    explainJson = json.loads(explain)
    qsn_tree, sbe_tree = parse_explain(explainJson)
    qsn_tree.print()
    sbe_tree.print()

    params = QueryParameters(10, 2000, "rooted-or")
    builder = ParametersBuilder()
    builder.process(explainJson, params)
    df = builder.buildDataFrame()
    print(df)
