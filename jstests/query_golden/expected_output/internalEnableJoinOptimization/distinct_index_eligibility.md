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
Execution Engine: sbe
```json
{
	"queryShapeHash" : "032E5B996E22D67FF5B909BB4D963F54372307CBECE35923143ACF815B7DE95B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"nss" : "test.distinct_index_eligibility_md",
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
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "IXSCAN"
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
						},
						"usedJoinOptimization" : false
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
						"stage" : "DISTINCT_SCAN",
						"usedJoinOptimization" : false
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
Execution Engine: sbe
```json
{
	"queryShapeHash" : "64CFE6E39DB3056D464E804B38D1199EFE2E317E40E1A66F213E6E9DEFBEAE49",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "COLLSCAN"
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
						"stage" : "DISTINCT_SCAN",
						"usedJoinOptimization" : false
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
			},
			"usedJoinOptimization" : false
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(3.0, inf]"
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
			"stage" : "DISTINCT_SCAN",
			"usedJoinOptimization" : false
		}
	]
}
```

### strict && sparse index => no DISTINCT_SCAN
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : null }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "C1D25B5AA65606D923F09EAA2D6FF66281A37C833C9B82149378ACA501211508",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### strict (with accum) && sparse index => no DISTINCT_SCAN
### Pipeline
```json
[
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
{  "_id" : null,  "accum" : 5 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "71E47B852DA989E1C35F92F980A09C6C4465B7D4BA0CF01A0FD321D59006F15C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### strict (with sort) && sparse index => no DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a"
		}
	}
]
```
### Results
```json
{  "_id" : null }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "1B8ACCF60E15794654C7F7C0CCB7ECD6DB28D3AC5D7D74537F7B5DD73CC9C7C2",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"memLimit" : 104857600,
			"sortPattern" : {
				"a" : 1
			},
			"stage" : "SORT",
			"type" : "simple"
		},
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false,
				"a" : true
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### strict && sparse index && alternative compound index => DISTINCT_SCAN on compound index
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : null }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "C1D25B5AA65606D923F09EAA2D6FF66281A37C833C9B82149378ACA501211508",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1
						},
						"usedJoinOptimization" : false
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
						"stage" : "DISTINCT_SCAN",
						"usedJoinOptimization" : false
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
				}
			}
		}
	]
}
```

### strict (with accum) && sparse index && alternative compound index => DISTINCT_SCAN on compound index
### Pipeline
```json
[
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
{  "_id" : null,  "accum" : 5 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "71E47B852DA989E1C35F92F980A09C6C4465B7D4BA0CF01A0FD321D59006F15C",
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
						},
						"usedJoinOptimization" : false
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
						"stage" : "DISTINCT_SCAN",
						"usedJoinOptimization" : false
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

### strict (with sort) && sparse index && alternative compound index => DISTINCT_SCAN on compound index
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a"
		}
	}
]
```
### Results
```json
{  "_id" : null }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "1B8ACCF60E15794654C7F7C0CCB7ECD6DB28D3AC5D7D74537F7B5DD73CC9C7C2",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1
						},
						"usedJoinOptimization" : false
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
						"stage" : "DISTINCT_SCAN",
						"usedJoinOptimization" : false
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
				}
			}
		}
	]
}
```

### strict (with sort and accum) && sparse index => no DISTINCT_SCAN
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
{  "_id" : null,  "accum" : 5 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "032E5B996E22D67FF5B909BB4D963F54372307CBECE35923143ACF815B7DE95B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"memLimit" : 104857600,
			"sortPattern" : {
				"a" : 1,
				"b" : 1
			},
			"stage" : "SORT",
			"type" : "simple"
		},
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false,
				"a" : true,
				"b" : true
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### strict (with sort and accum) && sparse index && alternative compound index => DISTINCT_SCAN on compound index
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
{  "_id" : null,  "accum" : 5 }
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
						},
						"usedJoinOptimization" : false
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"a" : [
								"[MaxKey, MinKey]"
							],
							"b" : [
								"[MaxKey, MinKey]"
							],
							"c" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "a_1_b_1_c_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1,
							"c" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ],
							"c" : [ ]
						},
						"stage" : "DISTINCT_SCAN",
						"usedJoinOptimization" : false
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

### !strict && sparse index => DISTINCT_SCAN
### Distinct on "a", with filter: { }
### Distinct results
`[ ]`
### Summarized explain
```json
{
	"queryShapeHash" : "6E9E554098A4B57378D683FDA4356A6158E56109968FEC6933BDA2CC789C09EF",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : 0,
				"a" : 1
			},
			"usedJoinOptimization" : false
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : true,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ]
			},
			"stage" : "DISTINCT_SCAN",
			"usedJoinOptimization" : false
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
			},
			"usedJoinOptimization" : false
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"$_path" : [
					"[\"a\", \"a\"]"
				],
				"a" : [
					"[-inf, 3.0)"
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
			"stage" : "DISTINCT_SCAN",
			"usedJoinOptimization" : false
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
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "COLLSCAN",
			"usedJoinOptimization" : false
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
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "FETCH",
			"usedJoinOptimization" : false
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"$_path" : [
					"[\"b\", \"b\"]"
				],
				"b" : [
					"[-inf, 5.0)"
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
			"nss" : "test.distinct_index_eligibility_md",
			"stage" : "IXSCAN",
			"usedJoinOptimization" : false
		}
	]
}
```

