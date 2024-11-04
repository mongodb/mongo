## 1. $sort + $group for unsharded collection
### Pipeline
```json
[
	{
		"$sort" : {
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Results
```json
{  "_id" : "shard0_1" }
{  "_id" : "shard0_2" }
{  "_id" : "shard0_3" }
{  "_id" : "shard1_1" }
{  "_id" : "shard1_2" }
{  "_id" : "shard1_3" }
```
### Summarized explain
```json
{
	"sort_group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"shardKey" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "shardKey_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"shardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$shardKey"
				}
			}
		}
	]
}
```

## 2. Push down $group preceded by $sort
### Pipeline
```json
[
	{
		"$sort" : {
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Results
```json
{  "_id" : "shard0_1" }
{  "_id" : "shard0_2" }
{  "_id" : "shard0_3" }
{  "_id" : "shard1_1" }
{  "_id" : "shard1_2" }
{  "_id" : "shard1_3" }
```
### Summarized explain
```json
{
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.sort_group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id"
			}
		}
	],
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey"
			}
		}
	],
	"sort_group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "shardKey_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey"
			}
		}
	],
	"sort_group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "shardKey_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey"
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$sort" : {
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"first" : {
				"$first" : "$otherField"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "shard0_1",  "first" : "a" }
{  "_id" : "shard0_2",  "first" : "b" }
{  "_id" : "shard0_3",  "first" : "c" }
{  "_id" : "shard1_1",  "first" : "a" }
{  "_id" : "shard1_2",  "first" : "b" }
{  "_id" : "shard1_3",  "first" : "c" }
```
### Summarized explain
```json
{
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.sort_group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"first" : {
					"$first" : "$$ROOT.first"
				}
			}
		}
	],
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey",
				"first" : {
					"$first" : "$otherField"
				}
			}
		}
	],
	"sort_group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"otherField" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"stage" : "FETCH"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "shardKey_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey",
				"first" : {
					"$first" : "$otherField"
				}
			}
		}
	],
	"sort_group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"otherField" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"stage" : "FETCH"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "shardKey_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey",
				"first" : {
					"$first" : "$otherField"
				}
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$project" : {
			"renamedShardKey" : "$shardKey",
			"otherField" : 1
		}
	},
	{
		"$sort" : {
			"renamedShardKey" : 1
		}
	},
	{
		"$match" : {
			"renamedShardKey" : {
				"$lte" : "shard1_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$renamedShardKey",
			"last" : {
				"$last" : "$otherField"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "shard0_1",  "last" : "a" }
{  "_id" : "shard0_2",  "last" : "b" }
{  "_id" : "shard0_3",  "last" : "c" }
{  "_id" : "shard1_1",  "last" : "a" }
```
### Summarized explain
```json
{
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.sort_group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"last" : {
					"$last" : "$$ROOT.last"
				}
			}
		}
	],
	"shardsPart" : [
		{
			"$match" : {
				"shardKey" : {
					"$lte" : "shard1_1"
				}
			}
		},
		{
			"$project" : {
				"_id" : true,
				"otherField" : true,
				"renamedShardKey" : "$shardKey"
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"renamedShardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"_id" : "$renamedShardKey",
				"last" : {
					"$last" : "$otherField"
				}
			}
		}
	],
	"sort_group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_DEFAULT",
						"transformBy" : {
							"_id" : true,
							"otherField" : true,
							"renamedShardKey" : "$shardKey"
						}
					},
					{
						"stage" : "FETCH"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[\"\", \"shard1_1\"]"
							]
						},
						"indexName" : "shardKey_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"renamedShardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"_id" : "$renamedShardKey",
				"last" : {
					"$last" : "$otherField"
				}
			}
		}
	],
	"sort_group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_DEFAULT",
						"transformBy" : {
							"_id" : true,
							"otherField" : true,
							"renamedShardKey" : "$shardKey"
						}
					},
					{
						"stage" : "FETCH"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[\"\", \"shard1_1\"]"
							]
						},
						"indexName" : "shardKey_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"renamedShardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"_id" : "$renamedShardKey",
				"last" : {
					"$last" : "$otherField"
				}
			}
		}
	]
}
```

## 3. Don't push down $group preceded by $sort if $group is not on shard key
### Pipeline
```json
[
	{
		"$sort" : {
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$otherField"
		}
	}
]
```
### Results
```json
{  "_id" : "a" }
{  "_id" : "b" }
{  "_id" : "c" }
```
### Summarized explain
```json
{
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.sort_group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"shardKey" : 1
				},
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"_id" : "$otherField"
			}
		}
	],
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : 1
				}
			}
		},
		{
			"$project" : {
				"_id" : false,
				"otherField" : true
			}
		}
	],
	"sort_group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"otherField" : true
				}
			},
			{
				"stage" : "SORT_KEY_GENERATOR"
			},
			{
				"stage" : "FETCH"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "shardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "IXSCAN"
			}
		]
	},
	"sort_group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"otherField" : true
				}
			},
			{
				"stage" : "SORT_KEY_GENERATOR"
			},
			{
				"stage" : "FETCH"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "shardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"shardKey" : 1
				},
				"multiKeyPaths" : {
					"shardKey" : [ ]
				},
				"stage" : "IXSCAN"
			}
		]
	}
}
```

## 4. Don't push down $group preceded by $sort if the shard key is not preserved
### Pipeline
```json
[
	{
		"$project" : {
			"shardKey" : "$otherField"
		}
	},
	{
		"$sort" : {
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Results
```json
{  "_id" : "a" }
{  "_id" : "b" }
{  "_id" : "c" }
```
### Summarized explain
```json
{
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.sort_group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"shardKey" : 1
				},
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey"
			}
		}
	],
	"shardsPart" : [
		{
			"$project" : {
				"_id" : true,
				"shardKey" : "$otherField"
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : 1
				}
			}
		}
	],
	"sort_group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_DEFAULT",
						"transformBy" : {
							"_id" : true,
							"shardKey" : "$otherField"
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : 1
				}
			}
		}
	],
	"sort_group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_DEFAULT",
						"transformBy" : {
							"_id" : true,
							"shardKey" : "$otherField"
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : 1
				}
			}
		}
	]
}
```

