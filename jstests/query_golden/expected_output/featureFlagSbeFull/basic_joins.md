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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "y",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (right-deep)
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
		"inputStages" : [
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			},
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x",
				"stage" : "HASH_JOIN_EMBEDDING"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "y",
		"planNodeId" : 5,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (zig-zag)
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "y",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "x",
				"rightEmbeddingField" : "none",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "y",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "x",
				"rightEmbeddingField" : "none",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "y",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "y",
				"rightEmbeddingField" : "none",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign1",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "x",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "y",
				"rightEmbeddingField" : "none",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign1",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "x",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With fixed order, index join
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "y",
				"rightEmbeddingField" : "none",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"inputStage" : {
					"indexName" : "a_1",
					"isMultiKey" : false,
					"isPartial" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"a" : 1
					},
					"nss" : "test.basic_joins_md_foreign1",
					"stage" : "INDEX_PROBE_NODE"
				},
				"nss" : "test.basic_joins_md_foreign1",
				"stage" : "FETCH"
			}
		],
		"joinPredicates" : [
			"a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 6,
		"rightEmbeddingField" : "x",
		"stage" : "INDEXED_NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"inputStages" : [
								{
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
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign1",
									"stage" : "COLLSCAN"
								}
							],
							"joinPredicates" : [
								"a = a"
							],
							"leftEmbeddingField" : "none",
							"rightEmbeddingField" : "x",
							"stage" : "HASH_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md_foreign2",
							"stage" : "COLLSCAN"
						}
					],
					"joinPredicates" : [
						"b = b"
					],
					"leftEmbeddingField" : "none",
					"planNodeId" : 6,
					"rightEmbeddingField" : "y",
					"stage" : "HASH_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

### With bottom-up plan enumeration (right-deep)
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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md_foreign2",
							"stage" : "COLLSCAN"
						},
						{
							"inputStages" : [
								{
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
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign1",
									"stage" : "COLLSCAN"
								}
							],
							"joinPredicates" : [
								"a = a"
							],
							"leftEmbeddingField" : "none",
							"rightEmbeddingField" : "x",
							"stage" : "HASH_JOIN_EMBEDDING"
						}
					],
					"joinPredicates" : [
						"b = b"
					],
					"leftEmbeddingField" : "y",
					"planNodeId" : 6,
					"rightEmbeddingField" : "none",
					"stage" : "HASH_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

### With bottom-up plan enumeration (zig-zag)
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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"inputStages" : [
								{
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
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign1",
									"stage" : "COLLSCAN"
								}
							],
							"joinPredicates" : [
								"a = a"
							],
							"leftEmbeddingField" : "none",
							"rightEmbeddingField" : "x",
							"stage" : "HASH_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md_foreign2",
							"stage" : "COLLSCAN"
						}
					],
					"joinPredicates" : [
						"b = b"
					],
					"leftEmbeddingField" : "none",
					"planNodeId" : 6,
					"rightEmbeddingField" : "y",
					"stage" : "HASH_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

### With random order, seed 44, nested loop joins
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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"inputStages" : [
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign1",
									"stage" : "COLLSCAN"
								},
								{
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
								}
							],
							"joinPredicates" : [
								"a = a"
							],
							"leftEmbeddingField" : "x",
							"rightEmbeddingField" : "none",
							"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md_foreign2",
							"stage" : "COLLSCAN"
						}
					],
					"joinPredicates" : [
						"b = b"
					],
					"leftEmbeddingField" : "none",
					"planNodeId" : 6,
					"rightEmbeddingField" : "y",
					"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

### With random order, seed 44, hash join enabled
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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"inputStages" : [
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign1",
									"stage" : "COLLSCAN"
								},
								{
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
								}
							],
							"joinPredicates" : [
								"a = a"
							],
							"leftEmbeddingField" : "x",
							"rightEmbeddingField" : "none",
							"stage" : "HASH_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md_foreign2",
							"stage" : "COLLSCAN"
						}
					],
					"joinPredicates" : [
						"b = b"
					],
					"leftEmbeddingField" : "none",
					"planNodeId" : 6,
					"rightEmbeddingField" : "y",
					"stage" : "HASH_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

### With random order, seed 420, nested loop joins
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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"inputStages" : [
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign2",
									"stage" : "COLLSCAN"
								},
								{
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
								}
							],
							"joinPredicates" : [
								"b = b"
							],
							"leftEmbeddingField" : "y",
							"rightEmbeddingField" : "none",
							"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md_foreign1",
							"stage" : "COLLSCAN"
						}
					],
					"joinPredicates" : [
						"a = a"
					],
					"leftEmbeddingField" : "none",
					"planNodeId" : 6,
					"rightEmbeddingField" : "x",
					"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

### With random order, seed 420, hash join enabled
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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"inputStages" : [
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign2",
									"stage" : "COLLSCAN"
								},
								{
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
								}
							],
							"joinPredicates" : [
								"b = b"
							],
							"leftEmbeddingField" : "y",
							"rightEmbeddingField" : "none",
							"stage" : "HASH_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								
							},
							"nss" : "test.basic_joins_md_foreign1",
							"stage" : "COLLSCAN"
						}
					],
					"joinPredicates" : [
						"a = a"
					],
					"leftEmbeddingField" : "none",
					"planNodeId" : 6,
					"rightEmbeddingField" : "x",
					"stage" : "HASH_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

### With fixed order, index join
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
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"inputStages" : [
						{
							"inputStages" : [
								{
									"direction" : "forward",
									"filter" : {
										
									},
									"nss" : "test.basic_joins_md_foreign2",
									"stage" : "COLLSCAN"
								},
								{
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
								}
							],
							"joinPredicates" : [
								"b = b"
							],
							"leftEmbeddingField" : "y",
							"rightEmbeddingField" : "none",
							"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
						},
						{
							"inputStage" : {
								"indexName" : "a_1",
								"isMultiKey" : false,
								"isPartial" : false,
								"isSparse" : false,
								"isUnique" : false,
								"keyPattern" : {
									"a" : 1
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "INDEX_PROBE_NODE"
							},
							"nss" : "test.basic_joins_md_foreign1",
							"stage" : "FETCH"
						}
					],
					"joinPredicates" : [
						"a = a"
					],
					"leftEmbeddingField" : "none",
					"planNodeId" : 7,
					"rightEmbeddingField" : "x",
					"stage" : "INDEXED_NESTED_LOOP_JOIN_EMBEDDING"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$y.b",
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
usedJoinOptimization: true

## 3. Basic example with referencing field from previous lookup
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.c = c"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (right-deep)
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
		"inputStages" : [
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			},
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x",
				"stage" : "HASH_JOIN_EMBEDDING"
			}
		],
		"joinPredicates" : [
			"c = x.c"
		],
		"leftEmbeddingField" : "z",
		"planNodeId" : 5,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (zig-zag)
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.c = c"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "x",
				"rightEmbeddingField" : "none",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.c = c"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "z",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"a = a"
				],
				"leftEmbeddingField" : "x",
				"rightEmbeddingField" : "none",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.c = c"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"c = c"
				],
				"leftEmbeddingField" : "z",
				"rightEmbeddingField" : "x",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "none",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"c = c"
				],
				"leftEmbeddingField" : "z",
				"rightEmbeddingField" : "x",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With fixed order, index join
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign1",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"c = c"
				],
				"leftEmbeddingField" : "z",
				"rightEmbeddingField" : "x",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 5,
		"rightEmbeddingField" : "none",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

## 4. Basic example with 3 joins & subsequent join referencing fields from previous lookups
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "y",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.c = c"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (right-deep)
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
		"inputStages" : [
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			},
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					},
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "y",
				"rightEmbeddingField" : "none",
				"stage" : "HASH_JOIN_EMBEDDING"
			}
		],
		"joinPredicates" : [
			"c = x.c"
		],
		"leftEmbeddingField" : "z",
		"planNodeId" : 7,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (zig-zag)
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
		"inputStages" : [
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign3",
				"stage" : "COLLSCAN"
			},
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "y",
				"stage" : "HASH_JOIN_EMBEDDING"
			}
		],
		"joinPredicates" : [
			"c = x.c"
		],
		"leftEmbeddingField" : "z",
		"planNodeId" : 7,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "z",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "y",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "z",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "y",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign3",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"c = c"
						],
						"leftEmbeddingField" : "x",
						"rightEmbeddingField" : "z",
						"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "none",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "y",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign3",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"c = c"
						],
						"leftEmbeddingField" : "x",
						"rightEmbeddingField" : "z",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "none",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "y",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With fixed order, index join
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign3",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"c = c"
						],
						"leftEmbeddingField" : "x",
						"rightEmbeddingField" : "z",
						"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.a = a"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "none",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"inputStage" : {
					"indexName" : "b_1",
					"isMultiKey" : false,
					"isPartial" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"b" : 1
					},
					"nss" : "test.basic_joins_md_foreign2",
					"stage" : "INDEX_PROBE_NODE"
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "FETCH"
			}
		],
		"joinPredicates" : [
			"b = b"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 8,
		"rightEmbeddingField" : "y",
		"stage" : "INDEXED_NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

## 5. Basic example with 3 joins & subsequent join referencing nested paths
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "w.y",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"w.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "k.y.z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (right-deep)
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
		"inputStages" : [
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			},
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					},
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					}
				],
				"joinPredicates" : [
					"c = x.c"
				],
				"leftEmbeddingField" : "w.y",
				"rightEmbeddingField" : "none",
				"stage" : "HASH_JOIN_EMBEDDING"
			}
		],
		"joinPredicates" : [
			"d = w.y.d"
		],
		"leftEmbeddingField" : "k.y.z",
		"planNodeId" : 7,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With bottom-up plan enumeration (zig-zag)
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
		"inputStages" : [
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			},
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "w.y",
				"stage" : "HASH_JOIN_EMBEDDING"
			}
		],
		"joinPredicates" : [
			"d = w.y.d"
		],
		"leftEmbeddingField" : "k.y.z",
		"planNodeId" : 7,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "w.y",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"w.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "k.y.z",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 44, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"a = a"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "x",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign3",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "w.y",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md_foreign2",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"w.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "k.y.z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, nested loop joins
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign3",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"c = c"
						],
						"leftEmbeddingField" : "x",
						"rightEmbeddingField" : "w.y",
						"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"w.y.d = d"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "k.y.z",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "none",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With random order, seed 420, hash join enabled
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign3",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"c = c"
						],
						"leftEmbeddingField" : "x",
						"rightEmbeddingField" : "w.y",
						"stage" : "HASH_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"w.y.d = d"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "k.y.z",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "none",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

### With fixed order, index join
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
		"inputStages" : [
			{
				"inputStages" : [
					{
						"inputStages" : [
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign1",
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"nss" : "test.basic_joins_md_foreign3",
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"c = c"
						],
						"leftEmbeddingField" : "x",
						"rightEmbeddingField" : "w.y",
						"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"nss" : "test.basic_joins_md_foreign2",
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"w.y.d = d"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "k.y.z",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"nss" : "test.basic_joins_md",
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.a = a"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "none",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```
usedJoinOptimization: true

