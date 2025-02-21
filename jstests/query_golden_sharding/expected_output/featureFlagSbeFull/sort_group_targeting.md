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
	"queryShapeHash" : "A0D809DC5D5E36730AAF02B6B22C243B1DBC2F1F906124AA12E24177FAB2CB96",
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
		}
	],
	"queryShapeHash" : "A0D809DC5D5E36730AAF02B6B22C243B1DBC2F1F906124AA12E24177FAB2CB96",
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
				"$willBeMerged" : false,
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
			"$group" : {
				"$willBeMerged" : false,
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
			"$group" : {
				"$willBeMerged" : false,
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
		}
	],
	"queryShapeHash" : "EA937E2417B3EB3C8886BC96A1516917C2619F47EA8A27F84CADC1686D4B1595",
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
				"$willBeMerged" : false,
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[MinKey, MaxKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		}
	],
	"queryShapeHash" : "3A0E17C147F04A6C1D8F3C6BC8305E68EF2C6EE8DD87A6897A0A8C30A51120F4",
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
				"$willBeMerged" : false,
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
				"$willBeMerged" : false,
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
				"$willBeMerged" : false,
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
				"$willBeMerged" : false,
				"_id" : "$otherField"
			}
		}
	],
	"queryShapeHash" : "74ED1BBD174E087FE2E6A8F436E355E9D91AEF6E758095F27C63CFDDABFD1CE9",
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
				"$willBeMerged" : false,
				"_id" : "$shardKey"
			}
		}
	],
	"queryShapeHash" : "B23115C58DB44E45F6DAF98C908D8EA36A23519C7926A2B9098D45BE2146A8F6",
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
					"shardKey" : 1
				}
			}
		}
	]
}
```

