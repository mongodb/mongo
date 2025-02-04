## 1. $group on shard key with $top/$bottom
### DISTINCT_SCAN stored as inactive plan
### Pipeline:
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"shardKey" : 1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### shard_filtering_plan_cache-rs0
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "forward",
				"indexBounds" : {
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			
		}
	},
	"isActive" : false,
	"planCacheKey" : "F9036CA2",
	"shard" : "shard_filtering_plan_cache-rs0"
}
```
### shard_filtering_plan_cache-rs1
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "forward",
				"indexBounds" : {
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			
		}
	},
	"isActive" : false,
	"planCacheKey" : "F9036CA2",
	"shard" : "shard_filtering_plan_cache-rs1"
}
```

### DISTINCT_SCAN used as active plan
### Pipeline:
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"shardKey" : 1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### shard_filtering_plan_cache-rs0
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "forward",
				"indexBounds" : {
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			
		}
	},
	"isActive" : true,
	"planCacheKey" : "F9036CA2",
	"shard" : "shard_filtering_plan_cache-rs0"
}
```
### shard_filtering_plan_cache-rs1
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "forward",
				"indexBounds" : {
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			
		}
	},
	"isActive" : true,
	"planCacheKey" : "F9036CA2",
	"shard" : "shard_filtering_plan_cache-rs1"
}
```

## 2. $group on shard key with $first/$last
### DISTINCT_SCAN stored as inactive plan
### Pipeline:
```json
[
	{
		"$sort" : {
			"shardKey" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$first" : "$shardKey"
			}
		}
	}
]
```
### shard_filtering_plan_cache-rs0
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "backward",
				"indexBounds" : {
					"shardKey" : [
						"[MaxKey, MinKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			"shardKey" : -1
		}
	},
	"isActive" : false,
	"planCacheKey" : "190642E6",
	"shard" : "shard_filtering_plan_cache-rs0"
}
```
### shard_filtering_plan_cache-rs1
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "backward",
				"indexBounds" : {
					"shardKey" : [
						"[MaxKey, MinKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			"shardKey" : -1
		}
	},
	"isActive" : false,
	"planCacheKey" : "190642E6",
	"shard" : "shard_filtering_plan_cache-rs1"
}
```

### DISTINCT_SCAN used as active plan
### Pipeline:
```json
[
	{
		"$sort" : {
			"shardKey" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$first" : "$shardKey"
			}
		}
	}
]
```
### shard_filtering_plan_cache-rs0
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "backward",
				"indexBounds" : {
					"shardKey" : [
						"[MaxKey, MinKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			"shardKey" : -1
		}
	},
	"isActive" : true,
	"planCacheKey" : "190642E6",
	"shard" : "shard_filtering_plan_cache-rs0"
}
```
### shard_filtering_plan_cache-rs1
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"inputStage" : {
				"direction" : "backward",
				"indexBounds" : {
					"shardKey" : [
						"[MaxKey, MinKey]"
					]
				},
				"indexName" : "shardKey_1",
				"indexVersion" : 2,
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : true,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			},
			"stage" : "SORT_KEY_GENERATOR"
		},
		"stage" : "PROJECTION_COVERED",
		"transformBy" : {
			
		}
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			"_id" : 0,
			"shardKey" : 1
		},
		"query" : {
			
		},
		"sort" : {
			"shardKey" : -1
		}
	},
	"isActive" : true,
	"planCacheKey" : "190642E6",
	"shard" : "shard_filtering_plan_cache-rs1"
}
```

### DISTINCT_SCAN stored as inactive plan
### Pipeline:
```json
[
	{
		"$sort" : {
			"shardKey" : 1,
			"notShardKey" : 1
		}
	},
	{
		"$match" : {
			"shardKey" : {
				"$gt" : "chunk1_s0"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"r" : {
				"$last" : "$$ROOT"
			}
		}
	}
]
```
### shard_filtering_plan_cache-rs0
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"direction" : "backward",
			"indexBounds" : {
				"notShardKey" : [
					"[MaxKey, MinKey]"
				],
				"shardKey" : [
					"({}, \"chunk1_s0\")"
				]
			},
			"indexName" : "shardKey_1_notShardKey_1",
			"indexVersion" : 2,
			"isFetching" : true,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : true,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"notShardKey" : 1,
				"shardKey" : 1
			},
			"multiKeyPaths" : {
				"notShardKey" : [ ],
				"shardKey" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		},
		"stage" : "SORT_KEY_GENERATOR"
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			
		},
		"query" : {
			"shardKey" : {
				"$gt" : "chunk1_s0"
			}
		},
		"sort" : {
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	"isActive" : false,
	"planCacheKey" : "FB4DA32A",
	"shard" : "shard_filtering_plan_cache-rs0"
}
```
### shard_filtering_plan_cache-rs1
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"direction" : "backward",
			"indexBounds" : {
				"notShardKey" : [
					"[MaxKey, MinKey]"
				],
				"shardKey" : [
					"({}, \"chunk1_s0\")"
				]
			},
			"indexName" : "shardKey_1_notShardKey_1",
			"indexVersion" : 2,
			"isFetching" : true,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : true,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"notShardKey" : 1,
				"shardKey" : 1
			},
			"multiKeyPaths" : {
				"notShardKey" : [ ],
				"shardKey" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		},
		"stage" : "SORT_KEY_GENERATOR"
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			
		},
		"query" : {
			"shardKey" : {
				"$gt" : "chunk1_s0"
			}
		},
		"sort" : {
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	"isActive" : false,
	"planCacheKey" : "FB4DA32A",
	"shard" : "shard_filtering_plan_cache-rs1"
}
```

### DISTINCT_SCAN used as active plan
### Pipeline:
```json
[
	{
		"$sort" : {
			"shardKey" : 1,
			"notShardKey" : 1
		}
	},
	{
		"$match" : {
			"shardKey" : {
				"$gt" : "chunk1_s0"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"r" : {
				"$last" : "$$ROOT"
			}
		}
	}
]
```
### shard_filtering_plan_cache-rs0
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"direction" : "backward",
			"indexBounds" : {
				"notShardKey" : [
					"[MaxKey, MinKey]"
				],
				"shardKey" : [
					"({}, \"chunk1_s0\")"
				]
			},
			"indexName" : "shardKey_1_notShardKey_1",
			"indexVersion" : 2,
			"isFetching" : true,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : true,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"notShardKey" : 1,
				"shardKey" : 1
			},
			"multiKeyPaths" : {
				"notShardKey" : [ ],
				"shardKey" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		},
		"stage" : "SORT_KEY_GENERATOR"
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			
		},
		"query" : {
			"shardKey" : {
				"$gt" : "chunk1_s0"
			}
		},
		"sort" : {
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	"isActive" : true,
	"planCacheKey" : "FB4DA32A",
	"shard" : "shard_filtering_plan_cache-rs0"
}
```
### shard_filtering_plan_cache-rs1
```json
{
	"cachedPlan" : {
		"inputStage" : {
			"direction" : "backward",
			"indexBounds" : {
				"notShardKey" : [
					"[MaxKey, MinKey]"
				],
				"shardKey" : [
					"({}, \"chunk1_s0\")"
				]
			},
			"indexName" : "shardKey_1_notShardKey_1",
			"indexVersion" : 2,
			"isFetching" : true,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : true,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"notShardKey" : 1,
				"shardKey" : 1
			},
			"multiKeyPaths" : {
				"notShardKey" : [ ],
				"shardKey" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		},
		"stage" : "SORT_KEY_GENERATOR"
	},
	"createdFromQuery" : {
		"distinct" : {
			"key" : "shardKey"
		},
		"projection" : {
			
		},
		"query" : {
			"shardKey" : {
				"$gt" : "chunk1_s0"
			}
		},
		"sort" : {
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	"isActive" : true,
	"planCacheKey" : "FB4DA32A",
	"shard" : "shard_filtering_plan_cache-rs1"
}
```

