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
```json
{
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
```json
{
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
```json
{
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
```json
{
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
```json
{
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
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"a" : 1
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
				"_id" : "$a"
			}
		}
	]
}
```

