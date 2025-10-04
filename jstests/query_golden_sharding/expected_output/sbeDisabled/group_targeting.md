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
	"queryShapeHash" : "09EC87EC1562264EEBD9FFB4AFCA462B420CE9D7D84761393C1EDFF22CC0DC52",
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
	"queryShapeHash" : "478B6A0368D57A8E91672794B842019CD3599311CF35C5BF57ADC37B3B32D884",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 1,
							"shardKey" : 1
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"avg" : {
					"$avg" : "$_id"
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
							"_id" : 1,
							"shardKey" : 1
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"avg" : {
					"$avg" : "$_id"
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
	"queryShapeHash" : "9A81DF86B99921BDA452D7B2B1C281B4DD92D7793B7A41490B713E2E6240ABA8",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$renamedShardKey"
			}
		}
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$renamedShardKey"
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
	"queryShapeHash" : "2383D49B22274AFD9D150CAAF21DEE74D13CECD29F757749126D14AB6A8A1259",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 1,
							"shardKey" : 1
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : [
					"$shardKey",
					"$_id"
				]
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
							"_id" : 1,
							"shardKey" : 1
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : [
					"$shardKey",
					"$_id"
				]
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
	"queryShapeHash" : "09640F28D8AD9D13A38E9B0AD3B6639B815174E1C2D6EA8056C9C98F379C4304",
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
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 1,
							"otherField" : 1,
							"shardKey" : 1
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 1,
							"otherField" : 1,
							"shardKey" : 1
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
	"queryShapeHash" : "744ECBEB61B075559F88DC0388215DE156259BB0F0064E78DDA8F8941EE745BA",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	"queryShapeHash" : "78D76645B1D16F553E5F8EE2980FAB231DCC0EF7F8EF99C7A4F9CEE10F0E5460",
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
							"_id" : 0,
							"shardKey" : 1
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
							"_id" : 0,
							"shardKey" : 1
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
	"queryShapeHash" : "C605458B8E6E7334D2C90BC2104F144FD6F0C35D1D81B1AD224B21D9569B3346",
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
							"_id" : 0,
							"otherField" : 1,
							"shardKey" : 1
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
							"_id" : 0,
							"otherField" : 1,
							"shardKey" : 1
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
	"queryShapeHash" : "01FA23646F5D8B9B4431EE11F599E449A1C20E7F13511FF8A8A3D584334FDBB9",
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
							"_id" : 1,
							"shardKey" : 1
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
							"_id" : 1,
							"shardKey" : 1
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
	"queryShapeHash" : "7D37D6CBCE761BFCFC3A2C9573ACFCF7D1A4AA1FAECBB63DBA95DCC5848764CC",
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
	"group_targeting-rs0" : [
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
	],
	"group_targeting-rs1" : [
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
	"queryShapeHash" : "801173738BA7069D3C0E0D81881FD43C5B89488ABF1BFFE408F46A06BBD3C6F6",
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
	"group_targeting-rs0" : [
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
	],
	"group_targeting-rs1" : [
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
	"queryShapeHash" : "F3459C97A782CC2BD58165A9EE9BB0E1F2CCC8D51597AABBF3C54BD884203868",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 1,
							"shardKey" : 1
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 1,
							"shardKey" : 1
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
				"num" : {
					"$sum" : "$$ROOT.num"
				}
			}
		}
	],
	"queryShapeHash" : "8815C6186EC271EF6EAD8060BE52DEA1E3BD053F10839B00FDA354341E8F59B4",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"otherField" : 1
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
			"$group" : {
				"_id" : "$otherField"
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
							"_id" : 0,
							"otherField" : 1
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
			"$group" : {
				"_id" : "$otherField"
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
	"queryShapeHash" : "6270E1992E2C5A949A3BF78D8A05435A314BD09755ABC29677C048A700397834",
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
	"group_targeting-rs0" : [
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
			"$group" : {
				"_id" : "$shardKey"
			}
		}
	],
	"group_targeting-rs1" : [
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
			"$group" : {
				"_id" : "$shardKey"
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
	"queryShapeHash" : "F8083EDFC37C8CC3733CEDD935E4B3C831389D112C8EE226B709FAF053BD938B",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
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
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
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
	"queryShapeHash" : "B1196288FCD6D08D7BCD65966BBBDE65B135237A5052F14820D01F6407A0F3A6",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"otherField" : 1
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
			"$addFields" : {
				"shardKey" : "$otherField"
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey"
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
							"_id" : 0,
							"otherField" : 1
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
			"$addFields" : {
				"shardKey" : "$otherField"
			}
		},
		{
			"$group" : {
				"_id" : "$shardKey"
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
	"queryShapeHash" : "1D7219367D726673E808E9ADABBA76A609AC8A44D104676C049DBCF4D99004B2",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
			"$group" : {
				"_id" : "$$ROOT"
			}
		}
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
			"$group" : {
				"_id" : "$$ROOT"
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
	"queryShapeHash" : "212EE000EB2F7C3432078C9D533F87A72EB238E2129907C7F4A4D2A699947658",
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
	"queryShapeHash" : "09DBC64EC8B53958B982EBE3BD1BE19E71C1317D623AC79678873C14E1403B14",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"shardKey" : 1
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
			"$group" : {
				"_id" : "$shardKey"
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
							"_id" : 0,
							"shardKey" : 1
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
			"$group" : {
				"_id" : "$shardKey"
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
	"queryShapeHash" : "B41F2EC9172F6DC814DF78027338CF30B2DAB99831594593CFEF245E4A09DF0E",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"otherField" : 1
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"otherField" : 1
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
	"queryShapeHash" : "B3C864E3B979A9C1B9204E3ED19C34C7FF2F405B7160A775B41011C9C1814BC4",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$sk0",
					"sk1" : "$sk1",
					"sk2" : "$sk2"
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
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
			"$group" : {
				"$willBeMerged" : false,
				"_id" : {
					"sk0" : "$sk0",
					"sk1" : "$sk1",
					"sk2" : "$sk2"
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
				"nss" : "test.group_targeting_compound",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "24ED0D35376542D41F0864F8C9CA57CEEB2C5349BC4D4B19E604D676D6F03ED2",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
	],
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
	"queryShapeHash" : "DAEAFF96C282BD9AD9C46355BBF7CC957C9F1422034E2A9628230E3418D1F7AA",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
	],
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
	"queryShapeHash" : "E14ADCE7FE8E28132D87B7342B3646B56A17778A7C09920810A13809BF0D0D5C",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	],
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
	"queryShapeHash" : "4A506A9019D292BBC65C1580E7334C4734704B2505A8D7E6D32DA2B49BE95CB3",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	],
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
	"queryShapeHash" : "B02FCF28584D92BB14174CAC9DA5147AAA724016A998B55B7C44231BCCFBE901",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk2" : 1
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
			"$group" : {
				"_id" : {
					"sk0" : "$sk0",
					"sk2" : "$sk2"
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
							"_id" : 0,
							"sk0" : 1,
							"sk2" : 1
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
			"$group" : {
				"_id" : {
					"sk0" : "$sk0",
					"sk2" : "$sk2"
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
	"queryShapeHash" : "E59D43DD14125FE5B51043359F995E95C252781A68D114AAFE19CB6EA235C1AA",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
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
						"stage" : "COLLSCAN"
					}
				]
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
	],
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
	"queryShapeHash" : "F914DD4E873964214B07A5ABD513F7CB3EDD9B0BF67E25A3C1B527974ADB64D6",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
	],
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
	"queryShapeHash" : "02C301C10BD18B30DE4781D29D1010E2929142A43962083EC483BE52763D2099",
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
	"group_targeting-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
	],
	"group_targeting-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"sk0" : 1,
							"sk1" : 1,
							"sk2" : 1
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
	],
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
	"queryShapeHash" : "C259DE523DB4694D046A9B1A41F3EE34A56E621DA571DAEF926E0DF41E45542E",
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

