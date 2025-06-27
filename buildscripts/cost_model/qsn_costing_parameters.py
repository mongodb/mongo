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

from typing import Any, Callable, Optional, TypeVar

import execution_tree_classic as classic
import execution_tree_sbe as sbe
import pandas as pd
import query_solution_tree as qsn
from workload_execution import QueryParameters

Node = TypeVar("Node")


def parse_explain_sbe(explain: dict[str, Any]) -> (qsn.Node, sbe.Node):
    qsn_tree = qsn.build(explain["queryPlanner"]["winningPlan"])
    sbe_tree = sbe.build_execution_tree(explain["executionStats"])
    return (qsn_tree, sbe_tree)


def parse_explain_classic(explain: dict[str, Any]) -> (qsn.Node, classic.Node):
    qsn_tree = qsn.build(explain["queryPlanner"]["winningPlan"])
    exec_tree = classic.build_execution_tree(explain["executionStats"])
    return (qsn_tree, exec_tree)


def parse_explain_classic(explain: dict[str, Any]) -> (qsn.Node, classic.Node):
    qsn_tree = qsn.build(explain["queryPlanner"]["winningPlan"])
    exec_tree = classic.build_execution_tree(explain["executionStats"])
    return (qsn_tree, exec_tree)


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


class ParametersBuilderClassic:
    """Prepare data for calibration from explain outputs."""

    def __init__(self):
        self.rows = []

    def process(self, explain: dict[str, Any], params: QueryParameters):
        qsn_tree, classic_tree = parse_explain_classic(explain)
        self._process(qsn_tree, classic_tree, params)

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

    def _process(self, qsn_node: qsn.Node, node: classic.Node, params: QueryParameters):
        self.rows.append(self._process_generic(qsn_node.node_type, node, params))
        # Set strict to true, as these should always have the same tree structure.
        for qsn_child, classic_child in zip(qsn_node.children, node.children, strict=True):
            self._process(qsn_child, classic_child, params)

    def _process_generic(self, stage: str, node: classic.Node, params: QueryParameters):
        return ParametersBuilderClassic._build_row(
            stage,
            params,
            execution_time=node.execution_time_nanoseconds,
            n_processed=node.n_processed,
            seeks=node.seeks,
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


class ParametersBuilderSBE:
    """Prepare data for calibration from explain outputs."""

    def __init__(self):
        self.processors = {}
        self.default_processor = self._process_generic
        self.rows = []

    def process(self, explain: dict[str, Any], params: QueryParameters):
        qsn_tree, sbe_tree = parse_explain_sbe(explain)
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
        return ParametersBuilderSBE._build_row(
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

    explain = r"""
    {
	"explainVersion" : "1",
	"queryPlanner" : {
		"namespace" : "test.and_sorted",
		"parsedQuery" : {
			"$or" : [
				{
					"$and" : [
						{
							"b" : {
								"$eq" : "1"
							}
						},
						{
							"a" : {
								"$in" : [
									"1",
									"2"
								]
							}
						}
					]
				},
				{
					"$and" : [
						{
							"d" : {
								"$eq" : "3"
							}
						},
						{
							"e" : {
								"$eq" : "3"
							}
						}
					]
				},
				{
					"$and" : [
						{
							"f" : {
								"$eq" : "4"
							}
						},
						{
							"g" : {
								"$eq" : "3"
							}
						}
					]
				}
			]
		},
		"indexFilterSet" : false,
		"queryHash" : "AD50C8FE",
		"planCacheShapeHash" : "AD50C8FE",
		"planCacheKey" : "F134F6EA",
		"optimizationTimeMillis" : 3,
		"maxIndexedOrSolutionsReached" : false,
		"maxIndexedAndSolutionsReached" : false,
		"maxScansToExplodeReached" : false,
		"prunedSimilarIndexes" : false,
		"winningPlan" : {
			"isCached" : false,
			"stage" : "SUBPLAN",
			"inputStage" : {
				"stage" : "FETCH",
				"inputStage" : {
					"stage" : "SORT_MERGE",
					"sortPattern" : {
						"c" : 1
					},
					"inputStages" : [
						{
							"stage" : "IXSCAN",
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"indexName" : "a_1_b_1_c_1",
							"isMultiKey" : false,
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"isUnique" : false,
							"isSparse" : false,
							"isPartial" : false,
							"indexVersion" : 2,
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"1\", \"1\"]"
								],
								"b" : [
									"[\"1\", \"1\"]"
								],
								"c" : [
									"[MinKey, MaxKey]"
								]
							}
						},
						{
							"stage" : "IXSCAN",
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"indexName" : "a_1_b_1_c_1",
							"isMultiKey" : false,
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"isUnique" : false,
							"isSparse" : false,
							"isPartial" : false,
							"indexVersion" : 2,
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"2\", \"2\"]"
								],
								"b" : [
									"[\"1\", \"1\"]"
								],
								"c" : [
									"[MinKey, MaxKey]"
								]
							}
						},
						{
							"stage" : "FETCH",
							"filter" : {
								"e" : {
									"$eq" : "3"
								}
							},
							"inputStage" : {
								"stage" : "IXSCAN",
								"keyPattern" : {
									"d" : 1,
									"c" : 1
								},
								"indexName" : "d_1_c_1",
								"isMultiKey" : false,
								"multiKeyPaths" : {
									"d" : [ ],
									"c" : [ ]
								},
								"isUnique" : false,
								"isSparse" : false,
								"isPartial" : false,
								"indexVersion" : 2,
								"direction" : "forward",
								"indexBounds" : {
									"d" : [
										"[\"3\", \"3\"]"
									],
									"c" : [
										"[MinKey, MaxKey]"
									]
								}
							}
						},
						{
							"stage" : "FETCH",
							"filter" : {
								"g" : {
									"$eq" : "3"
								}
							},
							"inputStage" : {
								"stage" : "IXSCAN",
								"keyPattern" : {
									"f" : 1,
									"c" : 1
								},
								"indexName" : "f_1_c_1",
								"isMultiKey" : false,
								"multiKeyPaths" : {
									"f" : [ ],
									"c" : [ ]
								},
								"isUnique" : false,
								"isSparse" : false,
								"isPartial" : false,
								"indexVersion" : 2,
								"direction" : "forward",
								"indexBounds" : {
									"f" : [
										"[\"4\", \"4\"]"
									],
									"c" : [
										"[MinKey, MaxKey]"
									]
								}
							}
						}
					]
				}
			}
		},
		"rejectedPlans" : [ ]
	},
	"executionStats" : {
		"executionSuccess" : true,
		"nReturned" : 10,
		"executionTimeMillis" : 5,
		"totalKeysExamined" : 10,
		"totalDocsExamined" : 19,
		"executionStages" : {
			"isCached" : false,
			"stage" : "SUBPLAN",
			"nReturned" : 10,
			"executionTimeMillisEstimate" : 3,
			"executionTimeMicros" : 3368,
			"executionTimeNanos" : 3368499,
			"works" : 25,
			"advanced" : 10,
			"needTime" : 14,
			"needYield" : 0,
			"saveState" : 0,
			"restoreState" : 0,
			"isEOF" : 1,
			"inputStage" : {
				"stage" : "FETCH",
				"nReturned" : 10,
				"executionTimeMillisEstimate" : 1,
				"executionTimeMicros" : 1046,
				"executionTimeNanos" : 1046324,
				"works" : 24,
				"advanced" : 10,
				"needTime" : 14,
				"needYield" : 0,
				"saveState" : 0,
				"restoreState" : 0,
				"isEOF" : 1,
				"docsExamined" : 10,
				"alreadyHasObj" : 9,
				"inputStage" : {
					"stage" : "SORT_MERGE",
					"nReturned" : 10,
					"executionTimeMillisEstimate" : 0,
					"executionTimeMicros" : 971,
					"executionTimeNanos" : 971927,
					"works" : 24,
					"advanced" : 10,
					"needTime" : 14,
					"needYield" : 0,
					"saveState" : 0,
					"restoreState" : 0,
					"isEOF" : 1,
					"sortPattern" : {
						"c" : 1
					},
					"dupsTested" : 10,
					"dupsDropped" : 0,
					"inputStages" : [
						{
							"stage" : "IXSCAN",
							"nReturned" : 0,
							"executionTimeMillisEstimate" : 0,
							"executionTimeMicros" : 111,
							"executionTimeNanos" : 111205,
							"works" : 1,
							"advanced" : 0,
							"needTime" : 0,
							"needYield" : 0,
							"saveState" : 0,
							"restoreState" : 0,
							"isEOF" : 1,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"indexName" : "a_1_b_1_c_1",
							"isMultiKey" : false,
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"isUnique" : false,
							"isSparse" : false,
							"isPartial" : false,
							"indexVersion" : 2,
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"1\", \"1\"]"
								],
								"b" : [
									"[\"1\", \"1\"]"
								],
								"c" : [
									"[MinKey, MaxKey]"
								]
							},
							"keysExamined" : 0,
							"seeks" : 1,
							"dupsTested" : 0,
							"dupsDropped" : 0
						},
						{
							"stage" : "IXSCAN",
							"nReturned" : 1,
							"executionTimeMillisEstimate" : 0,
							"executionTimeMicros" : 85,
							"executionTimeNanos" : 85605,
							"works" : 2,
							"advanced" : 1,
							"needTime" : 0,
							"needYield" : 0,
							"saveState" : 0,
							"restoreState" : 0,
							"isEOF" : 1,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"indexName" : "a_1_b_1_c_1",
							"isMultiKey" : false,
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"isUnique" : false,
							"isSparse" : false,
							"isPartial" : false,
							"indexVersion" : 2,
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"2\", \"2\"]"
								],
								"b" : [
									"[\"1\", \"1\"]"
								],
								"c" : [
									"[MinKey, MaxKey]"
								]
							},
							"keysExamined" : 1,
							"seeks" : 1,
							"dupsTested" : 0,
							"dupsDropped" : 0
						},
						{
							"stage" : "FETCH",
							"filter" : {
								"e" : {
									"$eq" : "3"
								}
							},
							"nReturned" : 8,
							"executionTimeMillisEstimate" : 0,
							"executionTimeMicros" : 446,
							"executionTimeNanos" : 446632,
							"works" : 9,
							"advanced" : 8,
							"needTime" : 0,
							"needYield" : 0,
							"saveState" : 0,
							"restoreState" : 0,
							"isEOF" : 1,
							"docsExamined" : 8,
							"alreadyHasObj" : 0,
							"inputStage" : {
								"stage" : "IXSCAN",
								"nReturned" : 8,
								"executionTimeMillisEstimate" : 0,
								"executionTimeMicros" : 258,
								"executionTimeNanos" : 258120,
								"works" : 9,
								"advanced" : 8,
								"needTime" : 0,
								"needYield" : 0,
								"saveState" : 0,
								"restoreState" : 0,
								"isEOF" : 1,
								"keyPattern" : {
									"d" : 1,
									"c" : 1
								},
								"indexName" : "d_1_c_1",
								"isMultiKey" : false,
								"multiKeyPaths" : {
									"d" : [ ],
									"c" : [ ]
								},
								"isUnique" : false,
								"isSparse" : false,
								"isPartial" : false,
								"indexVersion" : 2,
								"direction" : "forward",
								"indexBounds" : {
									"d" : [
										"[\"3\", \"3\"]"
									],
									"c" : [
										"[MinKey, MaxKey]"
									]
								},
								"keysExamined" : 8,
								"seeks" : 1,
								"dupsTested" : 0,
								"dupsDropped" : 0
							}
						},
						{
							"stage" : "FETCH",
							"filter" : {
								"g" : {
									"$eq" : "3"
								}
							},
							"nReturned" : 1,
							"executionTimeMillisEstimate" : 0,
							"executionTimeMicros" : 124,
							"executionTimeNanos" : 124694,
							"works" : 2,
							"advanced" : 1,
							"needTime" : 0,
							"needYield" : 0,
							"saveState" : 0,
							"restoreState" : 0,
							"isEOF" : 1,
							"docsExamined" : 1,
							"alreadyHasObj" : 0,
							"inputStage" : {
								"stage" : "IXSCAN",
								"nReturned" : 1,
								"executionTimeMillisEstimate" : 0,
								"executionTimeMicros" : 84,
								"executionTimeNanos" : 84292,
								"works" : 2,
								"advanced" : 1,
								"needTime" : 0,
								"needYield" : 0,
								"saveState" : 0,
								"restoreState" : 0,
								"isEOF" : 1,
								"keyPattern" : {
									"f" : 1,
									"c" : 1
								},
								"indexName" : "f_1_c_1",
								"isMultiKey" : false,
								"multiKeyPaths" : {
									"f" : [ ],
									"c" : [ ]
								},
								"isUnique" : false,
								"isSparse" : false,
								"isPartial" : false,
								"indexVersion" : 2,
								"direction" : "forward",
								"indexBounds" : {
									"f" : [
										"[\"4\", \"4\"]"
									],
									"c" : [
										"[MinKey, MaxKey]"
									]
								},
								"keysExamined" : 1,
								"seeks" : 1,
								"dupsTested" : 0,
								"dupsDropped" : 0
							}
						}
					]
				}
			}
		}
	},
	"queryShapeHash" : "ED0570742F8B713F6AB10101FE755DD90D901D507338F6191424BE1F16CC9C9D",
	"command" : {
		"find" : "and_sorted",
		"filter" : {
			"$or" : [
				{
					"a" : {
						"$in" : [
							"1",
							"2"
						]
					},
					"b" : "1"
				},
				{
					"d" : "3",
					"e" : "3"
				},
				{
					"f" : "4",
					"g" : "3"
				}
			]
		},
		"sort" : {
			"c" : 1
		},
		"$db" : "test"
	},
	"serverInfo" : {
		"host" : "ip-10-122-2-255",
		"port" : 27017,
		"version" : "8.2.0-alpha",
		"gitVersion" : "nogitversion"
	},
	"serverParameters" : {
		"internalQueryFacetBufferSizeBytes" : 104857600,
		"internalQueryFacetMaxOutputDocSizeBytes" : 104857600,
		"internalLookupStageIntermediateDocumentMaxSizeBytes" : 104857600,
		"internalDocumentSourceGroupMaxMemoryBytes" : 104857600,
		"internalQueryMaxBlockingSortMemoryUsageBytes" : 104857600,
		"internalQueryProhibitBlockingMergeOnMongoS" : 0,
		"internalQueryMaxAddToSetBytes" : 104857600,
		"internalDocumentSourceSetWindowFieldsMaxMemoryBytes" : 104857600,
		"internalQueryFrameworkControl" : "trySbeRestricted",
		"internalQueryPlannerIgnoreIndexWithCollationForRegex" : 1
	},
	"ok" : 1
    }
    """

    explainJson = json.loads(explain)
    qsn_tree, exec_tree = parse_explain_classic(explainJson)
    qsn_tree.print()
    exec_tree.print()

    params = QueryParameters(10, 2000, "rooted-or")
    builder = ParametersBuilderClassic()
    builder.process(explainJson, params)
    df = builder.buildDataFrame()
    print(df)
