## 1. Simple limit targeting multiple shards
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gt" : 30
		}
	},
	"limit" : 5
}
```
### Results
```json
5
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nCounted" : 5,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"isEOF" : 1,
						"nCounted" : 5,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 5
				},
				{
					"executionStages" : {
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
						"isEOF" : 1,
						"nCounted" : 5,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 5
				}
			],
			"stage" : "SHARD_MERGE",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 10
		}
	}
]
```

## 2. Simple skip targeting multiple shards
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gt" : 45,
			"$lt" : 55
		}
	},
	"skip" : 5
}
```
### Results
```json
4
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nCounted" : 4,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"nCounted" : 4,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 4
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"isEOF" : 1,
								"nReturned" : 5,
								"stage" : "IXSCAN"
							},
							"isEOF" : 1,
							"nReturned" : 5,
							"stage" : "SHARDING_FILTER"
						},
						"isEOF" : 1,
						"nCounted" : 5,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 5
				}
			],
			"skipAmount" : 5,
			"stage" : "SHARD_MERGE",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 9
		}
	}
]
```

## 3. Limit + skip targeting multiple shards
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gt" : 30
		}
	},
	"limit" : 5,
	"skip" : 5
}
```
### Results
```json
5
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nCounted" : 5,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"isEOF" : 1,
						"nCounted" : 10,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 10
				},
				{
					"executionStages" : {
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
						"isEOF" : 1,
						"nCounted" : 10,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 10
				}
			],
			"skipAmount" : 5,
			"stage" : "SHARD_MERGE",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 20
		}
	}
]
```

## 4. nCounted lower than limit
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gte" : 49,
			"$lte" : 51
		}
	},
	"limit" : 5
}
```
### Results
```json
3
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nCounted" : 3,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"nCounted" : 1,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 1
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"isEOF" : 1,
								"nReturned" : 2,
								"stage" : "IXSCAN"
							},
							"isEOF" : 1,
							"nReturned" : 2,
							"stage" : "SHARDING_FILTER"
						},
						"isEOF" : 1,
						"nCounted" : 2,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 2
				}
			],
			"stage" : "SHARD_MERGE",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 3
		}
	}
]
```

## 5. nCounted lower than skip + limit
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gte" : 47,
			"$lte" : 55
		}
	},
	"limit" : 5,
	"skip" : 5
}
```
### Results
```json
4
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"limitAmount" : 5,
			"nCounted" : 4,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"nCounted" : 3,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 3
				},
				{
					"executionStages" : {
						"inputStage" : {
							"inputStage" : {
								"isEOF" : 1,
								"nReturned" : 6,
								"stage" : "IXSCAN"
							},
							"isEOF" : 1,
							"nReturned" : 6,
							"stage" : "SHARDING_FILTER"
						},
						"isEOF" : 1,
						"nCounted" : 6,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 6
				}
			],
			"skipAmount" : 5,
			"stage" : "SHARD_MERGE",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 9
		}
	}
]
```

## 6. Simple limit targeting single shard
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gt" : 90
		}
	},
	"limit" : 5
}
```
### Results
```json
5
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nCounted" : 5,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"isEOF" : 1,
						"nCounted" : 5,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 5
				}
			],
			"stage" : "SINGLE_SHARD",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 5
		}
	}
]
```

## 7. Simple skip targeting single shard
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gt" : 90
		}
	},
	"skip" : 5
}
```
### Results
```json
4
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nCounted" : 4,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"nCounted" : 4,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 9
				}
			],
			"stage" : "SINGLE_SHARD",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 9
		}
	}
]
```

## 8. Simple limit + skip targeting single shard
### Query
```json
{
	"count" : "sharded_explain_count_with_limit_skip",
	"query" : {
		"x" : {
			"$gt" : 80
		}
	},
	"limit" : 5,
	"skip" : 5
}
```
### Results
```json
5
```
### Summarized explain executionStats
Execution Engine: classic
```json
[
	{
		"executionStages" : {
			"nCounted" : 5,
			"nReturned" : 0,
			"shards" : [
				{
					"executionStages" : {
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
						"isEOF" : 1,
						"nCounted" : 5,
						"nReturned" : 0,
						"stage" : "COUNT"
					},
					"nReturned" : 0,
					"totalDocsExamined" : 0,
					"totalKeysExamined" : 10
				}
			],
			"stage" : "SINGLE_SHARD",
			"totalDocsExamined" : 0,
			"totalKeysExamined" : 10
		}
	}
]
```

