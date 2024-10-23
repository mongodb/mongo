## 1. Find *without* collation on collection sharded on { "a" : 1 }
### Find : "{ "a" : "a" }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"indexBounds" : {
			"a" : [
				"[\"a\", \"a\"]"
			]
		},
		"indexName" : "a_1",
		"indexVersion" : 2,
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
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"indexName" : "_id_",
	"keyPattern" : "{ _id: 1 }",
	"stage" : "EXPRESS_IXSCAN"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

## 2. Find with collation on collection sharded on { "a" : 1 }
### Find : "{ "a" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"a" : {
						"$lte" : "a"
					}
				},
				{
					"a" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"_id" : {
						"$lte" : "a"
					}
				},
				{
					"_id" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

## 3. Find *without* collation on collection sharded on { "_id" : 1 }
### Find : "{ "a" : "a" }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"indexBounds" : {
			"a" : [
				"[\"a\", \"a\"]"
			]
		},
		"indexName" : "a_1",
		"indexVersion" : 2,
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
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"indexName" : "_id_",
	"keyPattern" : "{ _id: 1 }",
	"stage" : "EXPRESS_IXSCAN"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

## 4. Find with collation on collection sharded on { "_id" : 1 }
### Find : "{ "a" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"a" : {
						"$lte" : "a"
					}
				},
				{
					"a" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"_id" : {
						"$lte" : "a"
					}
				},
				{
					"_id" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

## 5. Find *without* collation on collection sharded on { "a" : "hashed" }
### Find : "{ "a" : "a" }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"indexBounds" : {
			"a" : [
				"[\"a\", \"a\"]"
			]
		},
		"indexName" : "a_1",
		"indexVersion" : 2,
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
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"indexName" : "_id_",
	"keyPattern" : "{ _id: 1 }",
	"stage" : "EXPRESS_IXSCAN"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

## 6. Find with collation on collection sharded on { "a" : "hashed" }
### Find : "{ "a" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"a" : {
						"$lte" : "a"
					}
				},
				{
					"a" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"_id" : {
						"$lte" : "a"
					}
				},
				{
					"_id" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

## 7. Find *without* collation on collection sharded on { "_id" : "hashed" }
### Find : "{ "a" : "a" }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"indexBounds" : {
			"a" : [
				"[\"a\", \"a\"]"
			]
		},
		"indexName" : "a_1",
		"indexVersion" : 2,
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
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SINGLE_SHARD"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"indexName" : "_id_",
	"keyPattern" : "{ _id: 1 }",
	"stage" : "EXPRESS_IXSCAN"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", {})"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

## 8. Find with collation on collection sharded on { "_id" : "hashed" }
### Find : "{ "a" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"a" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"a" : {
						"$lte" : "a"
					}
				},
				{
					"a" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "a" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "a_1",
			"indexVersion" : 2,
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
		},
		"stage" : "SHARDING_FILTER"
	},
	"stage" : "FETCH"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : "a" }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$eq" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$lte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"_id" : {
				"$gte" : "a"
			}
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gte" : "a", "$lte" : "a" } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"direction" : "forward",
		"filter" : {
			"$and" : [
				{
					"_id" : {
						"$lte" : "a"
					}
				},
				{
					"_id" : {
						"$gte" : "a"
					}
				}
			]
		},
		"stage" : "COLLSCAN"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey]"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

### Find : "{ "_id" : { "$gt" : { "$minKey" : 1 }, "$lt" : { "$maxKey" : 1 } } }", additional params: { "collation" : { "locale" : "en_US", "strength" : 2 } }
### Stage
`"SHARD_MERGE"`
### Shard winning plans
```json
{
	"inputStage" : {
		"inputStage" : {
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"(MinKey, MaxKey)"
				]
			},
			"indexName" : "_id_",
			"indexVersion" : 2,
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"stage" : "IXSCAN"
		},
		"stage" : "FETCH"
	},
	"stage" : "SHARDING_FILTER"
}
```
### Results
```json
[ { "_id" : "a", "a" : "a" } ]
```

