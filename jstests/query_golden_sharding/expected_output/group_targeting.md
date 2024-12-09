## 1. Pushdown $group on single shard-key
### Pushdown works for simple $group where _id == shard key
### Pipeline
```json
[ { "$group" : { "_id" : "$shardKey" } } ]
```
### Results
```json
{  "_id" : "sHaRd0_2" }
{  "_id" : "shARD1_3" }
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
	"group_targeting-rs0" : [
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
	],
	"group_targeting-rs1" : [
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
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey"
			}
		}
	]
}
```

### Pushdown works for pipeline prefix $group that is eligible for distinct scan
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"otherField" : {
				"$top" : {
					"output" : "$otherField",
					"sortBy" : {
						"shardKey" : 1
					}
				}
			}
		}
	},
	{
		"$match" : {
			"_id" : {
				"$lte" : "shard1_1"
			}
		}
	},
	{
		"$sort" : {
			"_id" : 1
		}
	}
]
```
### Results
```json
{  "_id" : "sHaRd0_2",  "otherField" : "b" }
{  "_id" : "shARD1_3",  "otherField" : "c" }
{  "_id" : "shard0_1",  "otherField" : "a" }
{  "_id" : "shard0_2",  "otherField" : "b" }
{  "_id" : "shard0_3",  "otherField" : "c" }
{  "_id" : "shard1_1",  "otherField" : "a" }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[\"\", \"shard1_1\"]"
							]
						},
						"indexName" : "shardKey_1",
						"isFetching" : true,
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
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"otherField" : {
					"$top" : {
						"output" : "$otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"_id" : 1
				}
			}
		}
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[\"\", \"shard1_1\"]"
							]
						},
						"indexName" : "shardKey_1",
						"isFetching" : true,
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
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"otherField" : {
					"$top" : {
						"output" : "$otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"_id" : 1
				}
			}
		}
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"_id" : 1
				},
				"tailableMode" : "normal"
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"otherField" : {
					"$top" : {
						"output" : "$otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"_id" : 1
				}
			}
		}
	]
}
```

## 2. Pushdown $group executes $avg correctly
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"avg" : {
				"$avg" : "$_id"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "sHaRd0_2",  "avg" : 2.5 }
{  "_id" : "shARD1_3",  "avg" : 6.5 }
{  "_id" : "shard0_1",  "avg" : 1.25 }
{  "_id" : "shard0_2",  "avg" : 2 }
{  "_id" : "shard0_3",  "avg" : 3.25 }
{  "_id" : "shard1_1",  "avg" : 4.25 }
{  "_id" : "shard1_2",  "avg" : 5 }
{  "_id" : "shard1_3",  "avg" : 6 }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"avg" : {
					"$avg" : "$_id"
				}
			}
		}
	]
}
```

### Pushdown works for simple $group where _id == shard key with a simple rename
### Pipeline
```json
[
	{
		"$project" : {
			"renamedShardKey" : "$shardKey"
		}
	},
	{
		"$group" : {
			"_id" : "$renamedShardKey"
		}
	}
]
```
### Results
```json
{  "_id" : "sHaRd0_2" }
{  "_id" : "shARD1_3" }
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
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : true,
					"renamedShardKey" : "$shardKey"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : true,
					"renamedShardKey" : "$shardKey"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$project" : {
				"_id" : true,
				"renamedShardKey" : "$shardKey"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$renamedShardKey"
			}
		}
	]
}
```

## 3. Pushdown works for simple $group on superset of shard-key
### Pipeline
```json
[ { "$group" : { "_id" : [ "$shardKey", "$_id" ] } } ]
```
### Results
```json
{  "_id" : [ "sHaRd0_2", 2.5 ] }
{  "_id" : [ "shARD1_3", 6.5 ] }
{  "_id" : [ "shard0_1", 1 ] }
{  "_id" : [ "shard0_1", 1.5 ] }
{  "_id" : [ "shard0_2", 2 ] }
{  "_id" : [ "shard0_3", 3 ] }
{  "_id" : [ "shard0_3", 3.5 ] }
{  "_id" : [ "shard1_1", 4 ] }
{  "_id" : [ "shard1_1", 4.5 ] }
{  "_id" : [ "shard1_2", 5 ] }
{  "_id" : [ "shard1_3", 6 ] }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : [
					"$shardKey",
					"$_id"
				]
			}
		}
	]
}
```

### Pushdown works for pipeline prefix $group on superset of shard key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : [
				"$shardKey",
				"$_id"
			],
			"otherField" : {
				"$top" : {
					"output" : "$otherField",
					"sortBy" : {
						"shardKey" : 1
					}
				}
			}
		}
	},
	{
		"$match" : {
			"_id" : {
				"$lte" : "shard1_1"
			}
		}
	},
	{
		"$sort" : {
			"_id" : 1
		}
	}
]
```
### Results
```json
{  "_id" : [ "sHaRd0_2", 2.5 ],  "otherField" : "b" }
{  "_id" : [ "shARD1_3", 6.5 ],  "otherField" : "c" }
{  "_id" : [ "shard0_1", 1 ],  "otherField" : "a" }
{  "_id" : [ "shard0_1", 1.5 ],  "otherField" : "A" }
{  "_id" : [ "shard0_2", 2 ],  "otherField" : "b" }
{  "_id" : [ "shard0_3", 3 ],  "otherField" : "c" }
{  "_id" : [ "shard0_3", 3.5 ],  "otherField" : "C" }
{  "_id" : [ "shard1_1", 4 ],  "otherField" : "a" }
{  "_id" : [ "shard1_1", 4.5 ],  "otherField" : "A" }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"filter" : {
							"_id" : {
								"$lte" : "shard1_1"
							}
						},
						"stage" : "MATCH"
					},
					{
						"stage" : "GROUP"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"_id" : 1
				}
			}
		}
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"filter" : {
							"_id" : {
								"$lte" : "shard1_1"
							}
						},
						"stage" : "MATCH"
					},
					{
						"stage" : "GROUP"
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"_id" : 1
				}
			}
		}
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"_id" : 1
				},
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : [
					"$shardKey",
					"$_id"
				],
				"otherField" : {
					"$top" : {
						"output" : "$otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		},
		{
			"$match" : {
				"_id" : {
					"$lte" : "shard1_1"
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"_id" : 1
				}
			}
		}
	]
}
```

### Pushdown works for simple $group on superset of shard key + rename
### Pipeline
```json
[
	{
		"$project" : {
			"renamedShardKey" : "$shardKey"
		}
	},
	{
		"$group" : {
			"_id" : {
				"secretlyShardKey" : "$renamedShardKey",
				"actualId" : "$_id"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "actualId" : 1,  "secretlyShardKey" : "shard0_1" } }
{  "_id" : {  "actualId" : 1.5,  "secretlyShardKey" : "shard0_1" } }
{  "_id" : {  "actualId" : 2,  "secretlyShardKey" : "shard0_2" } }
{  "_id" : {  "actualId" : 2.5,  "secretlyShardKey" : "sHaRd0_2" } }
{  "_id" : {  "actualId" : 3,  "secretlyShardKey" : "shard0_3" } }
{  "_id" : {  "actualId" : 3.5,  "secretlyShardKey" : "shard0_3" } }
{  "_id" : {  "actualId" : 4,  "secretlyShardKey" : "shard1_1" } }
{  "_id" : {  "actualId" : 4.5,  "secretlyShardKey" : "shard1_1" } }
{  "_id" : {  "actualId" : 5,  "secretlyShardKey" : "shard1_2" } }
{  "_id" : {  "actualId" : 6,  "secretlyShardKey" : "shard1_3" } }
{  "_id" : {  "actualId" : 6.5,  "secretlyShardKey" : "shARD1_3" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : true,
					"renamedShardKey" : "$shardKey"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : true,
					"renamedShardKey" : "$shardKey"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$project" : {
				"_id" : true,
				"renamedShardKey" : "$shardKey"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"actualId" : "$_id",
					"secretlyShardKey" : "$renamedShardKey"
				}
			}
		}
	]
}
```

### Only partial pushdown of $group on key derived from shard-key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"$min" : [
					"$shardKey",
					1
				]
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1 }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : false,
							"shardKey" : true
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						{
							"$const" : 1
						}
					]
				}
			}
		}
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : false,
							"shardKey" : true
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						{
							"$const" : 1
						}
					]
				}
			}
		}
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						{
							"$const" : 1
						}
					]
				}
			}
		}
	]
}
```

### Only partial pushdown of $group on key derived from shard-key, more complex case
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"$min" : [
					"$shardKey",
					1
				]
			},
			"otherField" : {
				"$top" : {
					"output" : "$otherField",
					"sortBy" : {
						"shardKey" : 1
					}
				}
			}
		}
	},
	{
		"$match" : {
			"_id" : {
				"$lte" : "shard1_1"
			}
		}
	},
	{
		"$sort" : {
			"_id" : 1
		}
	}
]
```
### Results
```json

```
### Summarized explain
```json
{
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : false,
							"otherField" : true,
							"shardKey" : true
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						{
							"$const" : 1
						}
					]
				},
				"otherField" : {
					"$top" : {
						"output" : "$otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : false,
							"otherField" : true,
							"shardKey" : true
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						{
							"$const" : 1
						}
					]
				},
				"otherField" : {
					"$top" : {
						"output" : "$otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"otherField" : {
					"$top" : {
						"output" : "$$ROOT.otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		},
		{
			"$match" : {
				"_id" : {
					"$lte" : "shard1_1"
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"_id" : 1
				}
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						{
							"$const" : 1
						}
					]
				},
				"otherField" : {
					"$top" : {
						"output" : "$otherField",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		}
	]
}
```

### Only partial pushdown of $group on key derived from shard-key, dependency on other field
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"$min" : [
					"$shardKey",
					"$_id"
				]
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1 }
{  "_id" : 1.5 }
{  "_id" : 2 }
{  "_id" : 2.5 }
{  "_id" : 3 }
{  "_id" : 3.5 }
{  "_id" : 4 }
{  "_id" : 4.5 }
{  "_id" : 5 }
{  "_id" : 6 }
{  "_id" : 6.5 }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : true,
							"shardKey" : true
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						"$_id"
					]
				}
			}
		}
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : true,
							"shardKey" : true
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"direction" : "forward",
						"filter" : {
							
						},
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						"$_id"
					]
				}
			}
		}
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$group" : {
				"_id" : {
					"$min" : [
						"$shardKey",
						"$_id"
					]
				}
			}
		}
	]
}
```

### With multiple $groups, pushdown first group when on shard key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"key" : "$shardKey",
				"other" : "$otherField"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$_id.other"
		}
	}
]
```
### Results
```json
{  "_id" : "A" }
{  "_id" : "C" }
{  "_id" : "a" }
{  "_id" : "b" }
{  "_id" : "c" }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"key" : "$shardKey",
					"other" : "$otherField"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$_id.other"
			}
		}
	]
}
```

### Multiple groups on shard key- could actually push both down
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"key" : "$shardKey",
				"other" : "$otherField"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$_id"
		}
	}
]
```
### Results
```json
{  "_id" : {  "key" : "sHaRd0_2",  "other" : "b" } }
{  "_id" : {  "key" : "shARD1_3",  "other" : "c" } }
{  "_id" : {  "key" : "shard0_1",  "other" : "A" } }
{  "_id" : {  "key" : "shard0_1",  "other" : "a" } }
{  "_id" : {  "key" : "shard0_2",  "other" : "b" } }
{  "_id" : {  "key" : "shard0_3",  "other" : "C" } }
{  "_id" : {  "key" : "shard0_3",  "other" : "c" } }
{  "_id" : {  "key" : "shard1_1",  "other" : "A" } }
{  "_id" : {  "key" : "shard1_1",  "other" : "a" } }
{  "_id" : {  "key" : "shard1_2",  "other" : "b" } }
{  "_id" : {  "key" : "shard1_3",  "other" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"key" : "$shardKey",
					"other" : "$otherField"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$_id"
			}
		}
	]
}
```

### Fully pushed down $group, followed by partially pushed down group
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"avg" : {
				"$avg" : "$_id"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$avg",
			"num" : {
				"$count" : {
					
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1.25,  "num" : 1 }
{  "_id" : 2,  "num" : 1 }
{  "_id" : 2.5,  "num" : 1 }
{  "_id" : 3.25,  "num" : 1 }
{  "_id" : 4.25,  "num" : 1 }
{  "_id" : 5,  "num" : 1 }
{  "_id" : 6,  "num" : 1 }
{  "_id" : 6.5,  "num" : 1 }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"num" : {
					"$sum" : "$$ROOT.num"
				}
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"avg" : {
					"$avg" : "$_id"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$avg",
				"num" : {
					"$sum" : {
						"$const" : 1
					}
				}
			}
		}
	]
}
```

### Don't fully pushdown $group on non-shard-key
### Pipeline
```json
[ { "$group" : { "_id" : "$otherField" } } ]
```
### Results
```json
{  "_id" : "A" }
{  "_id" : "C" }
{  "_id" : "a" }
{  "_id" : "b" }
{  "_id" : "c" }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$group" : {
				"_id" : "$otherField"
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
			"shardKey" : "$otherField"
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
{  "_id" : "A" }
{  "_id" : "C" }
{  "_id" : "a" }
{  "_id" : "b" }
{  "_id" : "c" }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
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
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
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
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$project" : {
				"_id" : true,
				"shardKey" : "$otherField"
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

### Don't fully pushdown $group when the shard key is not preserved
### Pipeline
```json
[
	{
		"$project" : {
			"otherField" : 1
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
{  "_id" : null }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : true,
					"otherField" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : true,
					"otherField" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$project" : {
				"_id" : true,
				"otherField" : true
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

### Don't fully pushdown $group when the shard key is overwritten by $addFields
### Pipeline
```json
[
	{
		"$addFields" : {
			"shardKey" : "$otherField"
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
{  "_id" : "A" }
{  "_id" : "C" }
{  "_id" : "a" }
{  "_id" : "b" }
{  "_id" : "c" }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"shardKey" : "$otherField"
				}
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"otherField" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"shardKey" : "$otherField"
				}
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"otherField" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$addFields" : {
				"shardKey" : "$otherField"
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

### Don't fully pushdown $group for _id on $$ROOT
### Pipeline
```json
[ { "$group" : { "_id" : "$$ROOT" } } ]
```
### Results
```json
{  "_id" : {  "_id" : 1,  "otherField" : "a",  "shardKey" : "shard0_1" } }
{  "_id" : {  "_id" : 1.5,  "otherField" : "A",  "shardKey" : "shard0_1" } }
{  "_id" : {  "_id" : 2,  "otherField" : "b",  "shardKey" : "shard0_2" } }
{  "_id" : {  "_id" : 2.5,  "otherField" : "b",  "shardKey" : "sHaRd0_2" } }
{  "_id" : {  "_id" : 3,  "otherField" : "c",  "shardKey" : "shard0_3" } }
{  "_id" : {  "_id" : 3.5,  "otherField" : "C",  "shardKey" : "shard0_3" } }
{  "_id" : {  "_id" : 4,  "otherField" : "a",  "shardKey" : "shard1_1" } }
{  "_id" : {  "_id" : 4.5,  "otherField" : "A",  "shardKey" : "shard1_1" } }
{  "_id" : {  "_id" : 5,  "otherField" : "b",  "shardKey" : "shard1_2" } }
{  "_id" : {  "_id" : 6,  "otherField" : "c",  "shardKey" : "shard1_3" } }
{  "_id" : {  "_id" : 6.5,  "otherField" : "c",  "shardKey" : "shARD1_3" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$group" : {
				"_id" : "$$ROOT"
			}
		}
	]
}
```

### Could pushdown $group for _id on $$ROOT.shardKey
### Pipeline
```json
[ { "$group" : { "_id" : "$$ROOT.shardKey" } } ]
```
### Results
```json
{  "_id" : "sHaRd0_2" }
{  "_id" : "shARD1_3" }
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
	"group_targeting-rs0" : [
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
	],
	"group_targeting-rs1" : [
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
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$$ROOT.shardKey"
			}
		}
	]
}
```

### Don't push down $group on shard key if collation of aggregation is non-simple
Note: If we have duplicate _ids in the output, that signals a bug here.
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	},
	{
		"$addFields" : {
			"_id" : {
				"$toLower" : "$_id"
			}
		}
	}
]
```
### Options
```json
{ "collation" : { "locale" : "en_US", "strength" : 2 } }
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
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
		},
		{
			"$addFields" : {
				"_id" : {
					"$toLower" : [
						"$_id"
					]
				}
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"_id" : "$shardKey"
			}
		}
	]
}
```

### Don't push $group fully down if shard-key field was added later.
### Pipeline
```json
[
	{
		"$project" : {
			"shardKey" : 0
		}
	},
	{
		"$addFields" : {
			"shardKey" : "$otherField"
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
{  "_id" : "A" }
{  "_id" : "C" }
{  "_id" : "a" }
{  "_id" : "b" }
{  "_id" : "c" }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"shardKey" : "$otherField"
				}
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"shardKey" : false
				}
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"otherField" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"shardKey" : "$otherField"
				}
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"shardKey" : false
				}
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"otherField" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting",
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
			"$project" : {
				"_id" : true,
				"shardKey" : false
			}
		},
		{
			"$addFields" : {
				"shardKey" : "$otherField"
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

## 4. Sharded collection with compound key
### Pushdown works for simple $group where _id == shard key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0",
				"sk1" : "$sk1",
				"sk2" : "$sk2"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$sk0",
					"sk1" : "$sk1",
					"sk2" : "$sk2"
				}
			}
		}
	]
}
```

### Pushdown works for simple $group where _id == shard key + accumulators
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0",
				"sk1" : "$sk1",
				"sk2" : "$sk2"
			},
			"sumSk1" : {
				"$sum" : "$sk1"
			},
			"setSk2" : {
				"$addToSet" : "$sk2"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" },  "setSk2" : [ "a" ],  "sumSk1" : 2 }
{  "_id" : {  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" },  "setSk2" : [ "a" ],  "sumSk1" : 2 }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" },  "setSk2" : [ "b" ],  "sumSk1" : 1 }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" },  "setSk2" : [ "z" ],  "sumSk1" : 1 }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" },  "setSk2" : [ "b" ],  "sumSk1" : 4 }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" },  "setSk2" : [ "z" ],  "sumSk1" : 2 }
{  "_id" : {  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" },  "setSk2" : [ "b" ],  "sumSk1" : 6 }
{  "_id" : {  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" },  "setSk2" : [ "c" ],  "sumSk1" : 5 }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$sk0",
					"sk1" : "$sk1",
					"sk2" : "$sk2"
				},
				"setSk2" : {
					"$addToSet" : "$sk2"
				},
				"sumSk1" : {
					"$sum" : "$sk1"
				}
			}
		}
	]
}
```

### Pushdown works for simple $group where _id == shard key + can use distinct scan
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0",
				"sk1" : "$sk1",
				"sk2" : "$sk2"
			},
			"root" : {
				"$bottom" : {
					"sortBy" : {
						"sk0" : 1,
						"sk1" : 1,
						"sk2" : 1
					},
					"output" : "$$ROOT"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" },  "root" : {  "_id" : 1,  "otherField" : "abc",  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" },  "root" : {  "_id" : 2,  "otherField" : "def",  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" },  "root" : {  "_id" : 5,  "otherField" : "def",  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" },  "root" : {  "_id" : 6,  "otherField" : "abc",  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" },  "root" : {  "_id" : 3,  "otherField" : "abc",  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" },  "root" : {  "_id" : 4,  "otherField" : "abc",  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" },  "root" : {  "_id" : 7,  "otherField" : "def",  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" },  "root" : {  "_id" : 8,  "otherField" : "ghi",  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$sk0",
					"sk1" : "$sk1",
					"sk2" : "$sk2"
				},
				"root" : {
					"$bottom" : {
						"output" : "$$ROOT",
						"sortBy" : {
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
						}
					}
				}
			}
		}
	]
}
```

### Pushdown works for simple $group where _id == shard key with a simple rename
### Pipeline
```json
[
	{
		"$project" : {
			"sk0Renamed" : "$sk0",
			"sk1Renamed" : "$sk1",
			"sk2Renamed" : "$sk2",
			"_id" : 0
		}
	},
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0Renamed",
				"sk1" : "$sk1Renamed",
				"sk2" : "$sk2Renamed"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : false,
					"sk0Renamed" : "$sk0",
					"sk1Renamed" : "$sk1",
					"sk2Renamed" : "$sk2"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : false,
					"sk0Renamed" : "$sk0",
					"sk1Renamed" : "$sk1",
					"sk2Renamed" : "$sk2"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$project" : {
				"_id" : false,
				"sk0Renamed" : "$sk0",
				"sk1Renamed" : "$sk1",
				"sk2Renamed" : "$sk2"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$sk0Renamed",
					"sk1" : "$sk1Renamed",
					"sk2" : "$sk2Renamed"
				}
			}
		}
	]
}
```

### Only partial pushdown for simple $group where _id == shard key with a non-simple rename
### Pipeline
```json
[
	{
		"$project" : {
			"sk0Renamed" : "$sk0",
			"sk1Renamed" : {
				"$add" : [
					1,
					"$sk1"
				]
			},
			"sk2Renamed" : "$sk2",
			"_id" : 0
		}
	},
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0Renamed",
				"sk1" : "$sk1Renamed",
				"sk2" : "$sk2Renamed"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0",  "sk1" : 3,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 3,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 4,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 6,  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : false,
					"sk0Renamed" : "$sk0",
					"sk1Renamed" : {
						"$add" : [
							{
								"$const" : 1
							},
							"$sk1"
						]
					},
					"sk2Renamed" : "$sk2"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : false,
					"sk0Renamed" : "$sk0",
					"sk1Renamed" : {
						"$add" : [
							{
								"$const" : 1
							},
							"$sk1"
						]
					},
					"sk2Renamed" : "$sk2"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
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
			"$project" : {
				"_id" : false,
				"sk0Renamed" : "$sk0",
				"sk1Renamed" : {
					"$add" : [
						{
							"$const" : 1
						},
						"$sk1"
					]
				},
				"sk2Renamed" : "$sk2"
			}
		},
		{
			"$group" : {
				"_id" : {
					"sk0" : "$sk0Renamed",
					"sk1" : "$sk1Renamed",
					"sk2" : "$sk2Renamed"
				}
			}
		}
	]
}
```

### Only partial pushdown for simple $group where _id is a subset of the shard key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0",
				"sk2" : "$sk2"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0/1",  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s1",  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s1",  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
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
			"$group" : {
				"_id" : {
					"sk0" : "$sk0",
					"sk2" : "$sk2"
				}
			}
		}
	]
}
```

### Pushdown works for simple $group where _id is a superset of the shard key with a simple rename
### Pipeline
```json
[
	{
		"$project" : {
			"sk0Renamed" : "$sk0",
			"sk1Renamed" : "$sk1",
			"sk2Renamed" : "$sk2",
			"_id" : 0,
			"otherField" : 1
		}
	},
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0Renamed",
				"sk1" : "$sk1Renamed",
				"sk2" : "$sk2Renamed",
				"otherField" : "$otherField"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "otherField" : "ABC",  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "otherField" : "DEF",  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "otherField" : "GEH",  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" } }
{  "_id" : {  "otherField" : "abc",  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" } }
{  "_id" : {  "otherField" : "abc",  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" } }
{  "_id" : {  "otherField" : "abc",  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "otherField" : "abc",  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" } }
{  "_id" : {  "otherField" : "def",  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" } }
{  "_id" : {  "otherField" : "def",  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" } }
{  "_id" : {  "otherField" : "def",  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "otherField" : "ghi",  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : false,
					"otherField" : true,
					"sk0Renamed" : "$sk0",
					"sk1Renamed" : "$sk1",
					"sk2Renamed" : "$sk2"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : false,
					"otherField" : true,
					"sk0Renamed" : "$sk0",
					"sk1Renamed" : "$sk1",
					"sk2Renamed" : "$sk2"
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$project" : {
				"_id" : false,
				"otherField" : true,
				"sk0Renamed" : "$sk0",
				"sk1Renamed" : "$sk1",
				"sk2Renamed" : "$sk2"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"otherField" : "$otherField",
					"sk0" : "$sk0Renamed",
					"sk1" : "$sk1Renamed",
					"sk2" : "$sk2Renamed"
				}
			}
		}
	]
}
```

### Multiple groups on shard key- could actually push both down
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0",
				"sk1" : "$sk1",
				"sk2" : "$sk2"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$_id"
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$sk0",
					"sk1" : "$sk1",
					"sk2" : "$sk2"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$_id"
			}
		}
	]
}
```

### Push down $group which includes a field that is not the shardKey and is not a simple rename of some other field
### Pipeline
```json
[
	{
		"$addFields" : {
			"complex" : {
				"$rand" : {
					
				}
			}
		}
	},
	{
		"$group" : {
			"_id" : {
				"sk0" : "$sk0",
				"complex" : "$complex",
				"sk1" : "$sk1",
				"sk2" : "$sk2"
			}
		}
	},
	{
		"$group" : {
			"_id" : {
				"sk0" : "$_id.sk0",
				"sk1" : "$_id.sk1",
				"sk2" : "$_id.sk2"
			}
		}
	}
]
```
### Results
```json
{  "_id" : {  "sk0" : "s0",  "sk1" : 1,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0",  "sk1" : 2,  "sk2" : "a" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 1,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s0/1",  "sk1" : 2,  "sk2" : "z" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 3,  "sk2" : "b" } }
{  "_id" : {  "sk0" : "s1",  "sk1" : 5,  "sk2" : "c" } }
```
### Summarized explain
```json
{
	"group_targeting-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"complex" : {
						"$rand" : {
							
						}
					}
				}
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"sk0" : true,
					"sk1" : true,
					"sk2" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"group_targeting-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"complex" : {
						"$rand" : {
							
						}
					}
				}
			},
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : false,
					"sk0" : true,
					"sk1" : true,
					"sk2" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"filter" : {
					
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.group_targeting_compound",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"shardsPart" : [
		{
			"$addFields" : {
				"complex" : {
					"$rand" : {
						
					}
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"complex" : "$complex",
					"sk0" : "$sk0",
					"sk1" : "$sk1",
					"sk2" : "$sk2"
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$_id.sk0",
					"sk1" : "$_id.sk1",
					"sk2" : "$_id.sk2"
				}
			}
		}
	]
}
```

