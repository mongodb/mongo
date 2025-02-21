## 1. Distinct Field part of the Index Key Pattern
### flip && multikey => no DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$last" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : [ 1, 2, 3 ],  "accum" : 5 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "032E5B996E22D67FF5B909BB4D963F54372307CBECE35923143ACF815B7DE95B",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isMultiKey" : true,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [
								"a"
							],
							"b" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$a",
				"accum" : {
					"$last" : "$b"
				}
			}
		}
	]
}
```

### flip && !multikey => DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$last" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "accum" : 5 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "032E5B996E22D67FF5B909BB4D963F54372307CBECE35923143ACF815B7DE95B",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"a" : [
								"[MaxKey, MinKey]"
							],
							"b" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
				}
			}
		}
	]
}
```

### !flip && strict && multikey on distinct field => no DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"output" : "$b",
					"sortBy" : {
						"a" : 1,
						"b" : 1
					}
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : [ 1, 2, 3 ],  "accum" : 5 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "64CFE6E39DB3056D464E804B38D1199EFE2E317E40E1A66F213E6E9DEFBEAE49",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$a",
				"accum" : {
					"$top" : {
						"output" : "$b",
						"sortBy" : {
							"a" : 1,
							"b" : 1
						}
					}
				}
			}
		}
	]
}
```

### !flip && strict && the index is only multikey on the non-distinct field => DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"output" : "$b",
					"sortBy" : {
						"a" : 1,
						"b" : 1
					}
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "accum" : [ 1, 2, 3 ] }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "64CFE6E39DB3056D464E804B38D1199EFE2E317E40E1A66F213E6E9DEFBEAE49",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : true,
						"isMultiKey" : true,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [
								"b"
							]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
				}
			}
		}
	]
}
```

### !flip && !strict && !multikey on distinct field => DISTINCT_SCAN
### Distinct on "a", with filter: { "a" : { "$gt" : 3 } }
### Distinct results
`[ ]`
### Summarized explain
```json
{
	"queryShapeHash" : "1B20F3793EBB0427565F8D4C8BA4FA1E5EADC999B6588BA21896EC90769B57BB",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : 0,
				"a" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(3.0, inf.0]"
				],
				"b" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1_b_1",
			"isFetching" : false,
			"isMultiKey" : true,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1,
				"b" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ],
				"b" : [
					"b"
				]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

## 2. Distinct Field not part of the Index Key Pattern
### wildcard && covered projection => DISTINCT_SCAN
### Distinct on "a", with filter: { "a" : { "$lt" : 3 } }
### Distinct results
`[ 1, 2 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "E533AB4142A054A3104112E9558EFA2E8B597532D3D2FEE269284CFBF1CC6250",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : 0,
				"a" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"$_path" : [
					"[\"a\", \"a\"]"
				],
				"a" : [
					"[-inf.0, 3.0)"
				]
			},
			"indexName" : "$**_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"$_path" : 1,
				"a" : 1
			},
			"multiKeyPaths" : {
				"$_path" : [ ],
				"a" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

### !wildcard => no DISTINCT_SCAN
### Distinct on "a", with filter: { "a" : { "$lt" : 3 } }
### Distinct results
`[ 1, 2 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "E533AB4142A054A3104112E9558EFA2E8B597532D3D2FEE269284CFBF1CC6250",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"direction" : "forward",
			"filter" : {
				"a" : {
					"$lt" : 3
				}
			},
			"stage" : "COLLSCAN"
		}
	]
}
```

### wildcard && !covered projection => no DISTINCT_SCAN
### Distinct on "a", with filter: { "b" : { "$lt" : 5 } }
### Distinct results
`[ 4 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "817405761FF62C71619482EDFA46189A4695E0E46F0CE7E03E42DEB8AEC40001",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"$_path" : [
					"[\"b\", \"b\"]"
				],
				"b" : [
					"[-inf.0, 5.0)"
				]
			},
			"indexName" : "$**_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"$_path" : 1,
				"b" : 1
			},
			"multiKeyPaths" : {
				"$_path" : [ ],
				"b" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

