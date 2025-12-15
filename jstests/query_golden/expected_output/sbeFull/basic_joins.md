## 1. Basic example with two joins
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{  "_id" : 1,  "a" : 1,  "b" : "bar",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } }
{  "_id" : 1,  "a" : 1,  "b" : "bar",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "2312073BEBB7A5E1754E2D03D9CB40B75C126DBF60867D4639E7E281C9E3DFAC",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"asField" : "y",
		"foreignCollection" : "test.basic_joins_md_foreign2",
		"foreignField" : "b",
		"inputStage" : {
			"asField" : "x",
			"foreignCollection" : "test.basic_joins_md_foreign1",
			"foreignField" : "a",
			"inputStage" : {
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			},
			"localField" : "a",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"localField" : "b",
		"planNodeId" : 3,
		"scanDirection" : "forward",
		"stage" : "EQ_LOOKUP_UNWIND",
		"strategy" : "HashJoin"
	}
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 2. Basic example with two joins and suffix
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$sortByCount" : "$y.b"
	}
]
```
### Results
```json
{  "_id" : "bar",  "count" : 6 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "8E0DF14D3DA69D8B1CF0361E869369F9C5E5F2F2A5D81998E6800BDA4F997B57",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStage" : {
			"inputStage" : {
				"asField" : "y",
				"foreignCollection" : "test.basic_joins_md_foreign2",
				"foreignField" : "b",
				"inputStage" : {
					"asField" : "x",
					"foreignCollection" : "test.basic_joins_md_foreign1",
					"foreignField" : "a",
					"inputStage" : {
						"inputStage" : {
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md",
							"stage" : "COLLSCAN"
						},
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : false,
							"a" : true,
							"b" : true
						}
					},
					"localField" : "a",
					"scanDirection" : "forward",
					"stage" : "EQ_LOOKUP_UNWIND",
					"strategy" : "HashJoin"
				},
				"localField" : "b",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"stage" : "GROUP"
		},
		"memLimit" : 104857600,
		"planNodeId" : 6,
		"sortPattern" : {
			"count" : -1
		},
		"stage" : "SORT",
		"type" : "default"
	}
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 3. Example with two joins, suffix, and sub-pipeline with un-correlated $match
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$sortByCount" : "$x.a"
	}
]
```
### Results
```json
{  "_id" : 1,  "count" : 2 }
{  "_id" : 2,  "count" : 2 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "39062D46AC66A096B98DB7B9AD3C198F432183DF3436553F59C3EC8B5252F1EA",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStage" : {
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					"planNodeId" : 2,
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : false,
						"a" : true,
						"b" : true
					}
				}
			}
		},
		{
			"$lookup" : {
				"as" : "x",
				"foreignField" : "a",
				"from" : "basic_joins_md_foreign1",
				"let" : {
					
				},
				"localField" : "a",
				"pipeline" : [
					{
						"$match" : {
							"d" : {
								"$lt" : 3
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$lookup" : {
				"as" : "y",
				"foreignField" : "b",
				"from" : "basic_joins_md_foreign2",
				"let" : {
					
				},
				"localField" : "b",
				"pipeline" : [
					{
						"$match" : {
							"b" : {
								"$gt" : "aaa"
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$x.a",
				"count" : {
					"$sum" : {
						"$const" : 1
					}
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"count" : -1
				}
			}
		}
	]
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  FETCH [test.basic_joins_md_foreign2]
  
  |
  IXSCAN [test.basic_joins_md_foreign2]
  keyPattern: {  "b" : 1 }
  indexName: "b_1"
  isMultiKey: false
  multiKeyPaths: {  "b" : [ ] }
  isUnique: false
  isSparse: false
  isPartial: false
  direction: "forward"
  indexBounds: {  "b" : [ "(\"aaa\", {})" ] }
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 4. Example with two joins and sub-pipeline with un-correlated $match
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{  "_id" : 1,  "a" : 1,  "b" : "bar",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } }
{  "_id" : 1,  "a" : 1,  "b" : "bar",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "6E1AD01BB023E5575DBF8997EA4E7E46A5BCCAA49D659AFD0AB18E0D1E797FF4",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.basic_joins_md",
					"planNodeId" : 1,
					"stage" : "COLLSCAN"
				}
			}
		},
		{
			"$lookup" : {
				"as" : "x",
				"foreignField" : "a",
				"from" : "basic_joins_md_foreign1",
				"let" : {
					
				},
				"localField" : "a",
				"pipeline" : [
					{
						"$match" : {
							"d" : {
								"$lt" : 3
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$lookup" : {
				"as" : "y",
				"foreignField" : "b",
				"from" : "basic_joins_md_foreign2",
				"let" : {
					
				},
				"localField" : "b",
				"pipeline" : [
					{
						"$match" : {
							"b" : {
								"$gt" : "aaa"
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		}
	]
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  FETCH [test.basic_joins_md_foreign2]
  
  |
  IXSCAN [test.basic_joins_md_foreign2]
  keyPattern: {  "b" : 1 }
  indexName: "b_1"
  isMultiKey: false
  multiKeyPaths: {  "b" : [ ] }
  isUnique: false
  isSparse: false
  isPartial: false
  direction: "forward"
  indexBounds: {  "b" : [ "(\"aaa\", {})" ] }
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 5. Example with two joins, suffix, and sub-pipeline with un-correlated $match and $match prefix
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gt" : 1
			}
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$sortByCount" : "$x.a"
	}
]
```
### Results
```json
{  "_id" : 2,  "count" : 2 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "7D59BE511C0D33652DB26A61DFFA0D36F47580971FB97DF9C3C4BC3A3F52F932",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStage" : {
						"direction" : "forward",
						"filter" : {
							"a" : {
								"$gt" : 1
							}
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					"planNodeId" : 2,
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : false,
						"a" : true,
						"b" : true
					}
				}
			}
		},
		{
			"$lookup" : {
				"as" : "x",
				"foreignField" : "a",
				"from" : "basic_joins_md_foreign1",
				"let" : {
					
				},
				"localField" : "a",
				"pipeline" : [
					{
						"$match" : {
							"d" : {
								"$lt" : 3
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$lookup" : {
				"as" : "y",
				"foreignField" : "b",
				"from" : "basic_joins_md_foreign2",
				"let" : {
					
				},
				"localField" : "b",
				"pipeline" : [
					{
						"$match" : {
							"b" : {
								"$gt" : "aaa"
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$x.a",
				"count" : {
					"$sum" : {
						"$const" : 1
					}
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"count" : -1
				}
			}
		}
	]
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: {  "a" : {  "$gt" : 1 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: {  "a" : {  "$gt" : 1 } }
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  FETCH [test.basic_joins_md_foreign2]
  
  |
  IXSCAN [test.basic_joins_md_foreign2]
  keyPattern: {  "b" : 1 }
  indexName: "b_1"
  isMultiKey: false
  multiKeyPaths: {  "b" : [ ] }
  isUnique: false
  isSparse: false
  isPartial: false
  direction: "forward"
  indexBounds: {  "b" : [ "(\"aaa\", {})" ] }
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  PROJECTION_SIMPLE
  transformBy: {  "a" : true,  "b" : true,  "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: {  "a" : {  "$gt" : 1 } }
  direction: "forward"
```
## 6. Example with two joins and sub-pipeline with un-correlated $match and $match prefix
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gt" : 1
			}
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "D2D2550CF530351E25FFDB1BB9DBD96DD0CE98C0FA4082BADF1F9BC9F7579A03",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"direction" : "forward",
					"filter" : {
						"a" : {
							"$gt" : 1
						}
					},
					"nss" : "test.basic_joins_md",
					"planNodeId" : 1,
					"stage" : "COLLSCAN"
				}
			}
		},
		{
			"$lookup" : {
				"as" : "x",
				"foreignField" : "a",
				"from" : "basic_joins_md_foreign1",
				"let" : {
					
				},
				"localField" : "a",
				"pipeline" : [
					{
						"$match" : {
							"d" : {
								"$lt" : 3
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$lookup" : {
				"as" : "y",
				"foreignField" : "b",
				"from" : "basic_joins_md_foreign2",
				"let" : {
					
				},
				"localField" : "b",
				"pipeline" : [
					{
						"$match" : {
							"b" : {
								"$gt" : "aaa"
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		}
	]
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: {  "a" : {  "$gt" : 1 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: {  "a" : {  "$gt" : 1 } }
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: {  "d" : {  "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: {  "b" : {  "$gt" : "aaa" } }
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: {  "a" : {  "$gt" : 1 } }
  |  direction: "forward"
  |
  FETCH [test.basic_joins_md_foreign2]
  
  |
  IXSCAN [test.basic_joins_md_foreign2]
  keyPattern: {  "b" : 1 }
  indexName: "b_1"
  isMultiKey: false
  multiKeyPaths: {  "b" : [ ] }
  isUnique: false
  isSparse: false
  isPartial: false
  direction: "forward"
  indexBounds: {  "b" : [ "(\"aaa\", {})" ] }
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  filter: {  "b" : {  "$gt" : "aaa" } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  filter: {  "d" : {  "$lt" : 3 } }
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.basic_joins_md]
  filter: {  "a" : {  "$gt" : 1 } }
  direction: "forward"
```
## 7. Basic example with referencing field from previous lookup
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign3",
			"as" : "z",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$z"
	}
]
```
### Results
```json
{  "_id" : 0,  "a" : 1,  "b" : "foo",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "z" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 } }
{  "_id" : 1,  "a" : 1,  "b" : "bar",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "z" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "z" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 },  "z" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "DC5CAF413E9B6B8B5E8B2D3FB91E4D546524BF2707BBE30472D5CA8E12F6637E",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"asField" : "z",
		"foreignCollection" : "test.basic_joins_md_foreign3",
		"foreignField" : "c",
		"inputStage" : {
			"asField" : "x",
			"foreignCollection" : "test.basic_joins_md_foreign1",
			"foreignField" : "a",
			"inputStage" : {
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			},
			"localField" : "a",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"localField" : "x.c",
		"planNodeId" : 3,
		"scanDirection" : "forward",
		"stage" : "EQ_LOOKUP_UNWIND",
		"strategy" : "HashJoin"
	}
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 8. Basic example with 3 joins & subsequent join referencing fields from previous lookups
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign3",
			"as" : "z",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$z"
	}
]
```
### Results
```json
{  "_id" : 1,  "a" : 1,  "b" : "bar",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 },  "z" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 } }
{  "_id" : 1,  "a" : 1,  "b" : "bar",  "x" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 },  "z" : {  "_id" : 0,  "a" : 1,  "c" : "zoo",  "d" : 1 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 },  "z" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 },  "z" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 },  "y" : {  "_id" : 0,  "b" : "bar",  "d" : 2 },  "z" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 } }
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 },  "y" : {  "_id" : 1,  "b" : "bar",  "d" : 6 },  "z" : {  "_id" : 2,  "a" : 2,  "c" : "x",  "d" : 3 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "DD24FFAE1F16C6C1F5B03939E0C642A797120A2585058C72554C9649BEA427AB",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"asField" : "z",
		"foreignCollection" : "test.basic_joins_md_foreign3",
		"foreignField" : "c",
		"inputStage" : {
			"asField" : "y",
			"foreignCollection" : "test.basic_joins_md_foreign2",
			"foreignField" : "b",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.basic_joins_md_foreign1",
				"foreignField" : "a",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.basic_joins_md",
					"stage" : "COLLSCAN"
				},
				"localField" : "a",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "b",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"localField" : "x.c",
		"planNodeId" : 4,
		"scanDirection" : "forward",
		"stage" : "EQ_LOOKUP_UNWIND",
		"strategy" : "HashJoin"
	}
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a = a]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "x"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [x.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [x.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: {  "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 9. Basic example with 3 joins & subsequent join referencing nested paths
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign3",
			"as" : "w.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$w.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "k.y.z",
			"localField" : "w.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$k.y.z"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "k" : {  "y" : {  "z" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } } },  "w" : {  "y" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 } },  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "9226A4FAA10C8790B27D9B5B865D46D0C58179A837F014E6289A34A6EBE76420",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"asField" : "k.y.z",
		"foreignCollection" : "test.basic_joins_md_foreign2",
		"foreignField" : "d",
		"inputStage" : {
			"asField" : "w.y",
			"foreignCollection" : "test.basic_joins_md_foreign3",
			"foreignField" : "c",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.basic_joins_md_foreign1",
				"foreignField" : "a",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.basic_joins_md",
					"stage" : "COLLSCAN"
				},
				"localField" : "a",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "x.c",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"localField" : "w.y.d",
		"planNodeId" : 4,
		"scanDirection" : "forward",
		"stage" : "EQ_LOOKUP_UNWIND",
		"strategy" : "HashJoin"
	}
}
```

### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [w.y.d = d]
leftEmbeddingField: "none"
rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "w.y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [d = w.y.d]
leftEmbeddingField: "k.y.z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [c = x.c]
  |  leftEmbeddingField: "w.y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a = a]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "x"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [d = w.y.d]
leftEmbeddingField: "k.y.z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [x.c = c]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "w.y"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44, nested loop joins
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [w.y.d = d]
leftEmbeddingField: "none"
rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [x.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "w.y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [w.y.d = d]
leftEmbeddingField: "none"
rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "w.y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45, nested loop joins
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "k.y.z"
  rightEmbeddingField: "w.y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 45, hash join enabled
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "k.y.z"
  rightEmbeddingField: "w.y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With fixed order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "k.y.z"
  rightEmbeddingField: "w.y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [w.y.d = d]
leftEmbeddingField: "none"
rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "w.y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: {  "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
