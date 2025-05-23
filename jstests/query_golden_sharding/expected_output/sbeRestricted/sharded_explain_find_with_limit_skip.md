## 1. Simple limit targeting multiple shards
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gt" : 30
		}
	},
	"limit" : 5,
	"singleBatch" : false,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 31,  "x" : 31 }
{  "_id" : 32,  "x" : 32 }
{  "_id" : 33,  "x" : 33 }
{  "_id" : 34,  "x" : 34 }
{  "_id" : 35,  "x" : 35 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nReturned" : 5,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 0,
											"nReturned" : 5,
											"stage" : "IXSCAN"
										},
										"isEOF" : 0,
										"nReturned" : 5,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 0,
									"nReturned" : 5,
									"stage" : "FETCH"
								},
								"isEOF" : 0,
								"nReturned" : 5,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 0,
							"nReturned" : 5,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 5,
						"nReturned" : 5,
						"stage" : "LIMIT"
					},
					"nReturned" : 5,
					"totalDocsExamined" : 5,
					"totalKeysExamined" : 5
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 0,
											"nReturned" : 24,
											"stage" : "IXSCAN"
										},
										"isEOF" : 0,
										"nReturned" : 5,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 0,
									"nReturned" : 5,
									"stage" : "FETCH"
								},
								"isEOF" : 0,
								"nReturned" : 5,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 0,
							"nReturned" : 5,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 5,
						"nReturned" : 5,
						"stage" : "LIMIT"
					},
					"nReturned" : 5,
					"totalDocsExamined" : 5,
					"totalKeysExamined" : 24
				}
			],
			"stage" : "SHARD_MERGE_SORT",
			"totalDocsExamined" : 10,
			"totalKeysExamined" : 29
		}
	}
]
```

## 2. Simple skip targeting multiple shards
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gt" : 45,
			"$lt" : 55
		}
	},
	"skip" : 5,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 51,  "x" : 51 }
{  "_id" : 52,  "x" : 52 }
{  "_id" : 53,  "x" : 53 }
{  "_id" : 54,  "x" : 54 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nReturned" : 4,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"isEOF" : 1,
										"nReturned" : 4,
										"stage" : "IXSCAN"
									},
									"isEOF" : 1,
									"nReturned" : 4,
									"stage" : "SHARDING_FILTER"
								},
								"isEOF" : 1,
								"nReturned" : 4,
								"stage" : "FETCH"
							},
							"isEOF" : 1,
							"nReturned" : 4,
							"stage" : "SORT_KEY_GENERATOR"
						},
						"isEOF" : 1,
						"nReturned" : 4,
						"stage" : "PROJECTION_DEFAULT"
					},
					"nReturned" : 4,
					"totalDocsExamined" : 4,
					"totalKeysExamined" : 4
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"isEOF" : 1,
										"nReturned" : 9,
										"stage" : "IXSCAN"
									},
									"isEOF" : 1,
									"nReturned" : 5,
									"stage" : "SHARDING_FILTER"
								},
								"isEOF" : 1,
								"nReturned" : 5,
								"stage" : "FETCH"
							},
							"isEOF" : 1,
							"nReturned" : 5,
							"stage" : "SORT_KEY_GENERATOR"
						},
						"isEOF" : 1,
						"nReturned" : 5,
						"stage" : "PROJECTION_DEFAULT"
					},
					"nReturned" : 5,
					"totalDocsExamined" : 5,
					"totalKeysExamined" : 9
				}
			],
			"skipAmount" : 5,
			"stage" : "SHARD_MERGE_SORT",
			"totalDocsExamined" : 9,
			"totalKeysExamined" : 13
		}
	}
]
```

## 3. Limit + skip targeting multiple shards
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gt" : 30
		}
	},
	"skip" : 5,
	"limit" : 5,
	"singleBatch" : false,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 36,  "x" : 36 }
{  "_id" : 37,  "x" : 37 }
{  "_id" : 38,  "x" : 38 }
{  "_id" : 39,  "x" : 39 }
{  "_id" : 40,  "x" : 40 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nReturned" : 5,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 0,
											"nReturned" : 10,
											"stage" : "IXSCAN"
										},
										"isEOF" : 0,
										"nReturned" : 10,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 0,
									"nReturned" : 10,
									"stage" : "FETCH"
								},
								"isEOF" : 0,
								"nReturned" : 10,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 0,
							"nReturned" : 10,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 10,
						"nReturned" : 10,
						"stage" : "LIMIT"
					},
					"nReturned" : 10,
					"totalDocsExamined" : 10,
					"totalKeysExamined" : 10
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 0,
											"nReturned" : 29,
											"stage" : "IXSCAN"
										},
										"isEOF" : 0,
										"nReturned" : 10,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 0,
									"nReturned" : 10,
									"stage" : "FETCH"
								},
								"isEOF" : 0,
								"nReturned" : 10,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 0,
							"nReturned" : 10,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 10,
						"nReturned" : 10,
						"stage" : "LIMIT"
					},
					"nReturned" : 10,
					"totalDocsExamined" : 10,
					"totalKeysExamined" : 29
				}
			],
			"skipAmount" : 5,
			"stage" : "SHARD_MERGE_SORT",
			"totalDocsExamined" : 20,
			"totalKeysExamined" : 39
		}
	}
]
```

## 4. nReturned lower than limit
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gte" : 49,
			"$lte" : 51
		}
	},
	"limit" : 5,
	"singleBatch" : false,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 49,  "x" : 49 }
{  "_id" : 50,  "x" : 50 }
{  "_id" : 51,  "x" : 51 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nReturned" : 3,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 1,
											"nReturned" : 1,
											"stage" : "IXSCAN"
										},
										"isEOF" : 1,
										"nReturned" : 1,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 1,
									"nReturned" : 1,
									"stage" : "FETCH"
								},
								"isEOF" : 1,
								"nReturned" : 1,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 1,
							"nReturned" : 1,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 5,
						"nReturned" : 1,
						"stage" : "LIMIT"
					},
					"nReturned" : 1,
					"totalDocsExamined" : 1,
					"totalKeysExamined" : 1
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 1,
											"nReturned" : 3,
											"stage" : "IXSCAN"
										},
										"isEOF" : 1,
										"nReturned" : 2,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 1,
									"nReturned" : 2,
									"stage" : "FETCH"
								},
								"isEOF" : 1,
								"nReturned" : 2,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 1,
							"nReturned" : 2,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 5,
						"nReturned" : 2,
						"stage" : "LIMIT"
					},
					"nReturned" : 2,
					"totalDocsExamined" : 2,
					"totalKeysExamined" : 3
				}
			],
			"stage" : "SHARD_MERGE_SORT",
			"totalDocsExamined" : 3,
			"totalKeysExamined" : 4
		}
	}
]
```

## 5. nReturned lower than skip + limit
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gte" : 47,
			"$lte" : 55
		}
	},
	"skip" : 5,
	"limit" : 5,
	"singleBatch" : false,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 52,  "x" : 52 }
{  "_id" : 53,  "x" : 53 }
{  "_id" : 54,  "x" : 54 }
{  "_id" : 55,  "x" : 55 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nReturned" : 4,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 1,
											"nReturned" : 3,
											"stage" : "IXSCAN"
										},
										"isEOF" : 1,
										"nReturned" : 3,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 1,
									"nReturned" : 3,
									"stage" : "FETCH"
								},
								"isEOF" : 1,
								"nReturned" : 3,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 1,
							"nReturned" : 3,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 10,
						"nReturned" : 3,
						"stage" : "LIMIT"
					},
					"nReturned" : 3,
					"totalDocsExamined" : 3,
					"totalKeysExamined" : 3
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"inputStage" : {
											"isEOF" : 1,
											"nReturned" : 9,
											"stage" : "IXSCAN"
										},
										"isEOF" : 1,
										"nReturned" : 6,
										"stage" : "SHARDING_FILTER"
									},
									"isEOF" : 1,
									"nReturned" : 6,
									"stage" : "FETCH"
								},
								"isEOF" : 1,
								"nReturned" : 6,
								"stage" : "SORT_KEY_GENERATOR"
							},
							"isEOF" : 1,
							"nReturned" : 6,
							"stage" : "PROJECTION_DEFAULT"
						},
						"isEOF" : 1,
						"limitAmount" : 10,
						"nReturned" : 6,
						"stage" : "LIMIT"
					},
					"nReturned" : 6,
					"totalDocsExamined" : 6,
					"totalKeysExamined" : 9
				}
			],
			"skipAmount" : 5,
			"stage" : "SHARD_MERGE_SORT",
			"totalDocsExamined" : 9,
			"totalKeysExamined" : 12
		}
	}
]
```

## 6. Simple limit targeting single shard
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gt" : 90
		}
	},
	"limit" : 5,
	"singleBatch" : false,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 91,  "x" : 91 }
{  "_id" : 92,  "x" : 92 }
{  "_id" : 93,  "x" : 93 }
{  "_id" : 94,  "x" : 94 }
{  "_id" : 95,  "x" : 95 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nReturned" : 5,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"isEOF" : 0,
									"nReturned" : 5,
									"stage" : "IXSCAN"
								},
								"isEOF" : 0,
								"nReturned" : 5,
								"stage" : "SHARDING_FILTER"
							},
							"isEOF" : 0,
							"nReturned" : 5,
							"stage" : "FETCH"
						},
						"isEOF" : 1,
						"limitAmount" : 5,
						"nReturned" : 5,
						"stage" : "LIMIT"
					},
					"nReturned" : 5,
					"totalDocsExamined" : 5,
					"totalKeysExamined" : 5
				}
			],
			"stage" : "SINGLE_SHARD",
			"totalDocsExamined" : 5,
			"totalKeysExamined" : 5
		}
	}
]
```

## 7. Simple skip targeting single shard
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gt" : 90
		}
	},
	"skip" : 5,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 96,  "x" : 96 }
{  "_id" : 97,  "x" : 97 }
{  "_id" : 98,  "x" : 98 }
{  "_id" : 99,  "x" : 99 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nReturned" : 4,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"isEOF" : 1,
									"nReturned" : 9,
									"stage" : "IXSCAN"
								},
								"isEOF" : 1,
								"nReturned" : 9,
								"stage" : "SHARDING_FILTER"
							},
							"isEOF" : 1,
							"nReturned" : 4,
							"skipAmount" : 5,
							"stage" : "SKIP"
						},
						"isEOF" : 1,
						"nReturned" : 4,
						"stage" : "FETCH"
					},
					"nReturned" : 4,
					"totalDocsExamined" : 4,
					"totalKeysExamined" : 9
				}
			],
			"stage" : "SINGLE_SHARD",
			"totalDocsExamined" : 4,
			"totalKeysExamined" : 9
		}
	}
]
```

## 8. Simple limit + skip targeting single shard
### Query
```json
{
	"find" : "sharded_explain_find_with_limit_skip",
	"filter" : {
		"x" : {
			"$gt" : 90
		}
	},
	"skip" : 5,
	"limit" : 5,
	"singleBatch" : false,
	"sort" : {
		"x" : 1
	}
}
```
### Results
```json
{  "_id" : 96,  "x" : 96 }
{  "_id" : 97,  "x" : 97 }
{  "_id" : 98,  "x" : 98 }
{  "_id" : 99,  "x" : 99 }
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nReturned" : 4,
			"shards" : [
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"inputStage" : {
									"inputStage" : {
										"isEOF" : 1,
										"nReturned" : 9,
										"stage" : "IXSCAN"
									},
									"isEOF" : 1,
									"nReturned" : 9,
									"stage" : "SHARDING_FILTER"
								},
								"isEOF" : 1,
								"nReturned" : 4,
								"skipAmount" : 5,
								"stage" : "SKIP"
							},
							"isEOF" : 1,
							"nReturned" : 4,
							"stage" : "FETCH"
						},
						"isEOF" : 1,
						"limitAmount" : 5,
						"nReturned" : 4,
						"stage" : "LIMIT"
					},
					"nReturned" : 4,
					"totalDocsExamined" : 4,
					"totalKeysExamined" : 9
				}
			],
			"stage" : "SINGLE_SHARD",
			"totalDocsExamined" : 4,
			"totalKeysExamined" : 9
		}
	}
]
```

