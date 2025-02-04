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
	"queryShapeHash" : "091EB94278A1F85267F3544B6ECD5890DFDB1D53FDE9A7D5FEEB911C428C4A54",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
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
	"queryShapeHash" : "091EB94278A1F85267F3544B6ECD5890DFDB1D53FDE9A7D5FEEB911C428C4A54",
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
Execution Engine: sbe
```json
{
	"queryShapeHash" : "9BC802D6FC495962065BF0847511778C78810FDE9598FA921AD057035FD6327F",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
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
	"queryShapeHash" : "9BC802D6FC495962065BF0847511778C78810FDE9598FA921AD057035FD6327F",
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
### Expected results
`[ ]`
### Distinct results
`[ ]`
### Summarized explain
```json
{
	"queryShapeHash" : "28EA60B44617FE1C60E96E928E1A442CB819DBD37D1B88B62234E5D52985D523",
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
### Expected results
`[ 1, 2 ]`
### Distinct results
`[ 1, 2 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "246577E6358122F9987A766BA54FD210A7CD32BD545D012BBF0E306FE8AAD0FB",
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
### Expected results
`[ 1, 2 ]`
### Distinct results
`[ 1, 2 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "246577E6358122F9987A766BA54FD210A7CD32BD545D012BBF0E306FE8AAD0FB",
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
### Expected results
`[ 4 ]`
### Distinct results
`[ 4 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "CD06A55A8CF0B75DEEF9069F1DF542FFA9797E2B886DF4BE69B8F9C0DE9BC071",
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
			"isSparse" : true,
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

