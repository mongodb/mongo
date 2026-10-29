## 1. Multiple suitable indexes generate a single DISTINCT_SCAN candidate
### Pipeline
```json
[
	{
		"$unwind" : {
			"path" : "$a",
			"preserveNullAndEmptyArrays" : true
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
{ "_id" : 1 }
{ "_id" : 2 }
{ "_id" : 3 }
{ "_id" : 7 }
{ "_id" : null }
```
### Total indexes on the collection
```json
[ "_id_", "a_1", "a_1_b_1" ]
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "7369A25E4B52DA5797E4FFD461F50CE164B53CC62F7CD4D3BFEA04A3342FA081",
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
							]
						},
						"indexName" : "a_1",
						"isFetching" : false,
						"isMultiKey" : true,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1
						},
						"multiKeyPaths" : {
							"a" : [
								"a"
							]
						},
						"stage" : "DISTINCT_SCAN",
						"unwindsArrays" : true,
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

## 2. A hinted index that requires a fetch is not converted to a DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$unwind" : {
			"path" : "$a",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$group" : {
			"_id" : "$a"
		}
	}
]
```
### Options
```json
{ "hint" : { "a" : 1, "b" : 1 } }
```
### Results
```json
{ "_id" : 1 }
{ "_id" : 2 }
{ "_id" : 3 }
{ "_id" : 7 }
{ "_id" : null }
```
### Total indexes on the collection
```json
[ "_id_", "a_1", "a_1_b_1" ]
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "7369A25E4B52DA5797E4FFD461F50CE164B53CC62F7CD4D3BFEA04A3342FA081",
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
						},
						"usedJoinOptimization" : false
					},
					{
						"nss" : "test.unwind_group_to_distinct_scan_multiplanning_md",
						"stage" : "FETCH",
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
						"nss" : "test.unwind_group_to_distinct_scan_multiplanning_md",
						"stage" : "IXSCAN",
						"usedJoinOptimization" : false
					}
				]
			}
		},
		{
			"$unwind" : {
				"path" : "$a",
				"preserveNullAndEmptyArrays" : true
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$a"
			}
		}
	]
}
```

## 3. A hint can force the suitable index
### Pipeline
```json
[
	{
		"$unwind" : {
			"path" : "$a",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$group" : {
			"_id" : "$a"
		}
	}
]
```
### Options
```json
{ "hint" : { "a" : 1 } }
```
### Results
```json
{ "_id" : 1 }
{ "_id" : 2 }
{ "_id" : 3 }
{ "_id" : 7 }
{ "_id" : null }
```
### Total indexes on the collection
```json
[ "_id_", "a_1", "a_1_b_1" ]
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "7369A25E4B52DA5797E4FFD461F50CE164B53CC62F7CD4D3BFEA04A3342FA081",
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
						},
						"usedJoinOptimization" : false
					},
					{
						"nss" : "test.unwind_group_to_distinct_scan_multiplanning_md",
						"stage" : "FETCH",
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
						"isMultiKey" : true,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1
						},
						"multiKeyPaths" : {
							"a" : [
								"a"
							]
						},
						"nss" : "test.unwind_group_to_distinct_scan_multiplanning_md",
						"stage" : "IXSCAN",
						"usedJoinOptimization" : false
					}
				]
			}
		},
		{
			"$unwind" : {
				"path" : "$a",
				"preserveNullAndEmptyArrays" : true
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$a"
			}
		}
	]
}
```

## 4. A whole index scan candidate multiplans against the DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$unwind" : {
			"path" : "$a",
			"preserveNullAndEmptyArrays" : true
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
{ "_id" : 1 }
{ "_id" : 2 }
{ "_id" : 3 }
{ "_id" : null }
```
### Total indexes on the collection
```json
[ "_id_", "b_1_a_1", "a_1" ]
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "B1FC3E856EACC9C5AE3947F65357563FC1F315A4C9CB8BF8E9AC856CDF9A94D4",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
							"indexName" : "b_1_a_1",
							"isMultiKey" : false,
							"isPartial" : false,
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
							"nss" : "test.unwind_group_to_distinct_scan_multiplanning_md_scalar",
							"stage" : "IXSCAN",
							"usedJoinOptimization" : false
						}
					]
				],
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
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ]
						},
						"stage" : "DISTINCT_SCAN",
						"unwindsArrays" : true,
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

