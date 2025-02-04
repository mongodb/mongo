## 1. Sort Pattern Added for $groupByDistinctScan
### Suitable Index for Sort => Distinct Scan
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
{  "_id" : 1,  "accum" : 1 }
{  "_id" : 2,  "accum" : 3 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "A8D462371CDE9BE554D607AD88916CF20C2B5633E0454E7FFF6F7D0A89C142CD",
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

### Suitable Index for Sort (Inverse Order) => Distinct Scan
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
{  "_id" : 1,  "accum" : 1 }
{  "_id" : 2,  "accum" : 3 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "A8D462371CDE9BE554D607AD88916CF20C2B5633E0454E7FFF6F7D0A89C142CD",
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
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_-1_b_-1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : -1,
							"b" : -1
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

### No Suitable Index for Sort => No Distinct Scan and No Blocking Sort
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
{  "_id" : 1,  "accum" : 1 }
{  "_id" : 2,  "accum" : 3 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "A8D462371CDE9BE554D607AD88916CF20C2B5633E0454E7FFF6F7D0A89C142CD",
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

### Suitable Index for Filter but Not for Sort => No Distinct Scan and No Blocking Sort
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gt" : 3
			}
		}
	},
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
{  "_id" : 5,  "accum" : 4 }
{  "_id" : 6,  "accum" : 7 }
{  "_id" : 7,  "accum" : 3 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "4D59D9B70CAA743C51507B7F4CF216F652E91F22553A942308E91D358F754C44",
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
					"(3.0, inf.0]"
				]
			},
			"indexName" : "a_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

## 2. Construction of Distinct Scan when No Sort and No Filter
### $group Stage with no $sort Stage and with suitable index => DISTINCT_SCAN
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 1 }
{  "_id" : 2 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "CA2B2C90B53877652CBF1F4F7692F1DA0FA9476BC770590F5D8BCC5820FB58BA",
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
						}
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
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
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

### $group Stage with no $sort Stage and with no suitable index => No DISTINCT_SCAN
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 1 }
{  "_id" : 2 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "CA2B2C90B53877652CBF1F4F7692F1DA0FA9476BC770590F5D8BCC5820FB58BA",
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

