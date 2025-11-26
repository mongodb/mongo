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

### With bottom-up plan enumeration
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "y",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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

### With bottom-up plan enumeration
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
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
									"stage" : "COLLSCAN"
								}
							],
							"joinPredicates" : [
								"b = b"
							],
							"leftEmbeddingField" : "none",
							"rightEmbeddingField" : "y",
							"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								
							},
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
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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

### With bottom-up plan enumeration
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
										"d" : {
											"$lt" : 3
										}
									},
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
								"b" : {
									"$gt" : "aaa"
								}
							},
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

### With random order, seed 44, nested loop joins
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
										"b" : {
											"$gt" : "aaa"
										}
									},
									"stage" : "COLLSCAN"
								}
							],
							"joinPredicates" : [
								"b = b"
							],
							"leftEmbeddingField" : "none",
							"rightEmbeddingField" : "y",
							"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								"d" : {
									"$lt" : 3
								}
							},
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

### With random order, seed 44, hash join enabled
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
										"b" : {
											"$gt" : "aaa"
										}
									},
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
								"d" : {
									"$lt" : 3
								}
							},
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

### With random order, seed 420, nested loop joins
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
										"d" : {
											"$lt" : 3
										}
									},
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
								"b" : {
									"$gt" : "aaa"
								}
							},
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

### With random order, seed 420, hash join enabled
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											
										},
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
										"d" : {
											"$lt" : 3
										}
									},
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
								"b" : {
									"$gt" : "aaa"
								}
							},
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

### With bottom-up plan enumeration
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"d" : {
								"$lt" : 3
							}
						},
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
					"b" : {
						"$gt" : "aaa"
					}
				},
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

### With random order, seed 44, nested loop joins
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"b" : {
								"$gt" : "aaa"
							}
						},
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "y",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					"d" : {
						"$lt" : 3
					}
				},
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

### With random order, seed 44, hash join enabled
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"b" : {
								"$gt" : "aaa"
							}
						},
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
					"d" : {
						"$lt" : 3
					}
				},
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

### With random order, seed 420, nested loop joins
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"d" : {
								"$lt" : 3
							}
						},
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
					"b" : {
						"$gt" : "aaa"
					}
				},
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

### With random order, seed 420, hash join enabled
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"d" : {
								"$lt" : 3
							}
						},
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
					"b" : {
						"$gt" : "aaa"
					}
				},
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

### With bottom-up plan enumeration
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											"a" : {
												"$gt" : 1
											}
										},
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
										"d" : {
											"$lt" : 3
										}
									},
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
								"b" : {
									"$gt" : "aaa"
								}
							},
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

### With random order, seed 44, nested loop joins
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											"a" : {
												"$gt" : 1
											}
										},
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
										"b" : {
											"$gt" : "aaa"
										}
									},
									"stage" : "COLLSCAN"
								}
							],
							"joinPredicates" : [
								"b = b"
							],
							"leftEmbeddingField" : "none",
							"rightEmbeddingField" : "y",
							"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
						},
						{
							"direction" : "forward",
							"filter" : {
								"d" : {
									"$lt" : 3
								}
							},
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

### With random order, seed 44, hash join enabled
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											"a" : {
												"$gt" : 1
											}
										},
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
										"b" : {
											"$gt" : "aaa"
										}
									},
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
								"d" : {
									"$lt" : 3
								}
							},
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

### With random order, seed 420, nested loop joins
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											"a" : {
												"$gt" : 1
											}
										},
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
										"d" : {
											"$lt" : 3
										}
									},
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
								"b" : {
									"$gt" : "aaa"
								}
							},
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

### With random order, seed 420, hash join enabled
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
					"inputStages" : [
						{
							"inputStages" : [
								{
									"inputStage" : {
										"direction" : "forward",
										"filter" : {
											"a" : {
												"$gt" : 1
											}
										},
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
										"d" : {
											"$lt" : 3
										}
									},
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
								"b" : {
									"$gt" : "aaa"
								}
							},
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

### With bottom-up plan enumeration
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							"a" : {
								"$gt" : 1
							}
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"d" : {
								"$lt" : 3
							}
						},
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
					"b" : {
						"$gt" : "aaa"
					}
				},
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

### With random order, seed 44, nested loop joins
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							"a" : {
								"$gt" : 1
							}
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"b" : {
								"$gt" : "aaa"
							}
						},
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"b = b"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "y",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					"d" : {
						"$lt" : 3
					}
				},
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

### With random order, seed 44, hash join enabled
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							"a" : {
								"$gt" : 1
							}
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"b" : {
								"$gt" : "aaa"
							}
						},
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
					"d" : {
						"$lt" : 3
					}
				},
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

### With random order, seed 420, nested loop joins
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							"a" : {
								"$gt" : 1
							}
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"d" : {
								"$lt" : 3
							}
						},
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
					"b" : {
						"$gt" : "aaa"
					}
				},
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

### With random order, seed 420, hash join enabled
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
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStages" : [
			{
				"inputStages" : [
					{
						"direction" : "forward",
						"filter" : {
							"a" : {
								"$gt" : 1
							}
						},
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							"d" : {
								"$lt" : 3
							}
						},
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
					"b" : {
						"$gt" : "aaa"
					}
				},
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

### With bottom-up plan enumeration
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
						"stage" : "COLLSCAN"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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

### With bottom-up plan enumeration
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
								"stage" : "COLLSCAN"
							}
						],
						"joinPredicates" : [
							"b = b"
						],
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "y",
						"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
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
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.c = c"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "z",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```

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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
			"as" : "x.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$x.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "x.y.z",
			"localField" : "x.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$x.y.z"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "y" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "z" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } } } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "4545A5343C4F418AA879D5C8E031D6B637DFEE78E362FFC2EE47775E87790625",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"asField" : "x.y.z",
		"foreignCollection" : "test.basic_joins_md_foreign2",
		"foreignField" : "d",
		"inputStage" : {
			"asField" : "x.y",
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
		"localField" : "x.y.d",
		"planNodeId" : 4,
		"scanDirection" : "forward",
		"stage" : "EQ_LOOKUP_UNWIND",
		"strategy" : "HashJoin"
	}
}
```

### With bottom-up plan enumeration
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
			"as" : "x.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$x.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "x.y.z",
			"localField" : "x.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$x.y.z"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "y" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "z" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } } } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "4545A5343C4F418AA879D5C8E031D6B637DFEE78E362FFC2EE47775E87790625",
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x.y",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "x.y.z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```

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
			"as" : "x.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$x.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "x.y.z",
			"localField" : "x.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$x.y.z"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "y" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "z" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } } } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "4545A5343C4F418AA879D5C8E031D6B637DFEE78E362FFC2EE47775E87790625",
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x.y",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "x.y.z",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```

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
			"as" : "x.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$x.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "x.y.z",
			"localField" : "x.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$x.y.z"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "y" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "z" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } } } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "4545A5343C4F418AA879D5C8E031D6B637DFEE78E362FFC2EE47775E87790625",
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x.y",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "x.y.z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```

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
			"as" : "x.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$x.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "x.y.z",
			"localField" : "x.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$x.y.z"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "y" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "z" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } } } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "4545A5343C4F418AA879D5C8E031D6B637DFEE78E362FFC2EE47775E87790625",
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x.y",
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "x.y.z",
		"stage" : "NESTED_LOOP_JOIN_EMBEDDING"
	}
}
```

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
			"as" : "x.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$x.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "x.y.z",
			"localField" : "x.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$x.y.z"
	}
]
```
### Results
```json
{  "_id" : 2,  "a" : 2,  "b" : "bar",  "x" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "y" : {  "_id" : 1,  "a" : 2,  "c" : "blah",  "d" : 2,  "z" : {  "_id" : 0,  "b" : "bar",  "d" : 2 } } } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "4545A5343C4F418AA879D5C8E031D6B637DFEE78E362FFC2EE47775E87790625",
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
								"stage" : "COLLSCAN"
							},
							{
								"direction" : "forward",
								"filter" : {
									
								},
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
						"stage" : "COLLSCAN"
					}
				],
				"joinPredicates" : [
					"x.c = c"
				],
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "x.y",
				"stage" : "HASH_JOIN_EMBEDDING"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		],
		"joinPredicates" : [
			"x.y.d = d"
		],
		"leftEmbeddingField" : "none",
		"planNodeId" : 7,
		"rightEmbeddingField" : "x.y.z",
		"stage" : "HASH_JOIN_EMBEDDING"
	}
}
```

