## 1. With readConcern 'available' set via command options
### distinct on shard key
### Distinct on "shardKey", with filter: { }, and options: { "readConcern" : { "level" : "available" } }
### Expected results
`[ "chunk1_s0_0", "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Distinct results
`[ "chunk1_s0_0", "chunk1_s0_0_orphan", "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
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
		},
		"distinct_scan_read_concern_available-rs1" : {
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
	}
}
```

### Distinct on "shardKey", with filter: { "shardKey" : { "$gte" : "chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "available" } }
### Expected results
`[ "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Distinct results
`[ "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [
				[
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
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
					}
				]
			],
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
							"[\"chunk1_s0_1\", {})"
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
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [
				[
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
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
					}
				]
			],
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
							"[\"chunk1_s0_1\", {})"
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
	}
}
```

### Distinct on "shardKey", with filter: { "notShardKey" : { "$gte" : "1notShardKey_chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "available" } }
### Expected results
`[ "chunk1_s0_0", "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Distinct results
`[ "chunk1_s0_0", "chunk1_s0_0_orphan", "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$gte" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$gte" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		}
	}
}
```

### $group on shard key
### Pipeline
```json
[
	{
		"$match" : {
			
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "available" } }
```
### Results
```json
{  "_id" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_0_orphan" }
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_1_orphan" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s0_2_orphan" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_0_orphan" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_1_orphan" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk1_s1_2_orphan" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_0_orphan" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_1_orphan" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s0_2_orphan" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_0_orphan" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_1_orphan" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk2_s1_2_orphan" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_0_orphan" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_1_orphan" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s0_2_orphan" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_0_orphan" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_1_orphan" }
{  "_id" : "chunk3_s1_2" }
{  "_id" : "chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
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
	],
	"distinct_scan_read_concern_available-rs1" : [
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
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.distinct_scan_read_concern_available",
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
		"$match" : {
			"shardKey" : {
				"$gte" : "chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "available" } }
```
### Results
```json
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_1_orphan" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s0_2_orphan" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_0_orphan" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_1_orphan" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk1_s1_2_orphan" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_0_orphan" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_1_orphan" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s0_2_orphan" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_0_orphan" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_1_orphan" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk2_s1_2_orphan" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_0_orphan" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_1_orphan" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s0_2_orphan" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_0_orphan" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_1_orphan" }
{  "_id" : "chunk3_s1_2" }
{  "_id" : "chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[\"chunk1_s0_1\", {})"
								]
							},
							"indexName" : "shardKey_1_notShardKey_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
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
						}
					]
				],
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
								"[\"chunk1_s0_1\", {})"
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
	],
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[\"chunk1_s0_1\", {})"
								]
							},
							"indexName" : "shardKey_1_notShardKey_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
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
						}
					]
				],
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
								"[\"chunk1_s0_1\", {})"
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
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"shardKey" : {
					"$gte" : "chunk1_s0_1"
				}
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
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "available" } }
```
### Results
```json
{  "_id" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_0_orphan" }
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_1_orphan" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s0_2_orphan" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_0_orphan" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_1_orphan" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk1_s1_2_orphan" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_0_orphan" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_1_orphan" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s0_2_orphan" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_0_orphan" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_1_orphan" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk2_s1_2_orphan" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_0_orphan" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_1_orphan" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s0_2_orphan" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_0_orphan" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_1_orphan" }
{  "_id" : "chunk3_s1_2" }
{  "_id" : "chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"direction" : "forward",
				"filter" : {
					"notShardKey" : {
						"$gte" : "1notShardKey_chunk1_s0_1"
					}
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"distinct_scan_read_concern_available-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"direction" : "forward",
				"filter" : {
					"notShardKey" : {
						"$gte" : "1notShardKey_chunk1_s0_1"
					}
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"notShardKey" : {
					"$gte" : "1notShardKey_chunk1_s0_1"
				}
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

### distinct on non-shard key field
### Distinct on "notShardKey", with filter: { }, and options: { "readConcern" : { "level" : "available" } }
### Expected results
`[ "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_0_orphan", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

### Distinct on "notShardKey", with filter: { "shardKey" : { "$gte" : "chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "available" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [
				[
					{
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
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
			],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						],
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
						]
					},
					"indexName" : "shardKey_1_notShardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
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
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [
				[
					{
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
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
			],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						],
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
						]
					},
					"indexName" : "shardKey_1_notShardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
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
				}
			]
		}
	}
}
```

### Distinct on "notShardKey", with filter: { "notShardKey" : { "$gte" : "1notShardKey_chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "available" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_0_orphan", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", {})"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", {})"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

### $group on non-shard key field
### Pipeline
```json
[
	{
		"$match" : {
			
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "available" } }
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
{  "_id" : "notShardKey_chunk1_s0_0_orphan" }
{  "_id" : "notShardKey_chunk1_s0_1_orphan" }
{  "_id" : "notShardKey_chunk1_s0_2_orphan" }
{  "_id" : "notShardKey_chunk1_s1_0_orphan" }
{  "_id" : "notShardKey_chunk1_s1_1_orphan" }
{  "_id" : "notShardKey_chunk1_s1_2_orphan" }
{  "_id" : "notShardKey_chunk2_s0_0_orphan" }
{  "_id" : "notShardKey_chunk2_s0_1_orphan" }
{  "_id" : "notShardKey_chunk2_s0_2_orphan" }
{  "_id" : "notShardKey_chunk2_s1_0_orphan" }
{  "_id" : "notShardKey_chunk2_s1_1_orphan" }
{  "_id" : "notShardKey_chunk2_s1_2_orphan" }
{  "_id" : "notShardKey_chunk3_s0_0_orphan" }
{  "_id" : "notShardKey_chunk3_s0_1_orphan" }
{  "_id" : "notShardKey_chunk3_s0_2_orphan" }
{  "_id" : "notShardKey_chunk3_s1_0_orphan" }
{  "_id" : "notShardKey_chunk3_s1_1_orphan" }
{  "_id" : "notShardKey_chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
				"_id" : "$notShardKey"
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$match" : {
			"shardKey" : {
				"$gte" : "chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "available" } }
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
{  "_id" : "notShardKey_chunk1_s0_1_orphan" }
{  "_id" : "notShardKey_chunk1_s0_2_orphan" }
{  "_id" : "notShardKey_chunk1_s1_0_orphan" }
{  "_id" : "notShardKey_chunk1_s1_1_orphan" }
{  "_id" : "notShardKey_chunk1_s1_2_orphan" }
{  "_id" : "notShardKey_chunk2_s0_0_orphan" }
{  "_id" : "notShardKey_chunk2_s0_1_orphan" }
{  "_id" : "notShardKey_chunk2_s0_2_orphan" }
{  "_id" : "notShardKey_chunk2_s1_0_orphan" }
{  "_id" : "notShardKey_chunk2_s1_1_orphan" }
{  "_id" : "notShardKey_chunk2_s1_2_orphan" }
{  "_id" : "notShardKey_chunk3_s0_0_orphan" }
{  "_id" : "notShardKey_chunk3_s0_1_orphan" }
{  "_id" : "notShardKey_chunk3_s0_2_orphan" }
{  "_id" : "notShardKey_chunk3_s1_0_orphan" }
{  "_id" : "notShardKey_chunk3_s1_1_orphan" }
{  "_id" : "notShardKey_chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"stage" : "FETCH"
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
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
		],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[\"chunk1_s0_1\", {})"
					]
				},
				"indexName" : "shardKey_1_notShardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		]
	},
	"distinct_scan_read_concern_available-rs1" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"stage" : "FETCH"
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
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
		],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[\"chunk1_s0_1\", {})"
					]
				},
				"indexName" : "shardKey_1_notShardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"shardKey" : {
					"$gte" : "chunk1_s0_1"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$notShardKey"
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "available" } }
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
{  "_id" : "notShardKey_chunk1_s0_0_orphan" }
{  "_id" : "notShardKey_chunk1_s0_1_orphan" }
{  "_id" : "notShardKey_chunk1_s0_2_orphan" }
{  "_id" : "notShardKey_chunk1_s1_0_orphan" }
{  "_id" : "notShardKey_chunk1_s1_1_orphan" }
{  "_id" : "notShardKey_chunk1_s1_2_orphan" }
{  "_id" : "notShardKey_chunk2_s0_0_orphan" }
{  "_id" : "notShardKey_chunk2_s0_1_orphan" }
{  "_id" : "notShardKey_chunk2_s0_2_orphan" }
{  "_id" : "notShardKey_chunk2_s1_0_orphan" }
{  "_id" : "notShardKey_chunk2_s1_1_orphan" }
{  "_id" : "notShardKey_chunk2_s1_2_orphan" }
{  "_id" : "notShardKey_chunk3_s0_0_orphan" }
{  "_id" : "notShardKey_chunk3_s0_1_orphan" }
{  "_id" : "notShardKey_chunk3_s0_2_orphan" }
{  "_id" : "notShardKey_chunk3_s1_0_orphan" }
{  "_id" : "notShardKey_chunk3_s1_1_orphan" }
{  "_id" : "notShardKey_chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[\"1notShardKey_chunk1_s0_1\", {})"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[\"1notShardKey_chunk1_s0_1\", {})"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"notShardKey" : {
					"$gte" : "1notShardKey_chunk1_s0_1"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$notShardKey"
			}
		}
	]
}
```

## 2. With 'available' as the default readConcern
> [!NOTE]
> Explain doesn't support default readConcern, so a sharding filter stage may appear in the explain output even though the queries return orphans.
### distinct on shard key
### Distinct on "shardKey", with filter: { }
### Expected results
`[ "chunk1_s0_0", "chunk1_s0_0_orphan", "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Distinct results
`[ "chunk1_s0_0", "chunk1_s0_0_orphan", "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
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
		},
		"distinct_scan_read_concern_available-rs1" : {
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
	}
}
```

### Distinct on "shardKey", with filter: { "shardKey" : { "$gte" : "chunk1_s0_1" } }
### Expected results
`[ "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Distinct results
`[ "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [
				[
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
						"isFetching" : false,
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
					}
				]
			],
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
							"[\"chunk1_s0_1\", {})"
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
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [
				[
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
						"isFetching" : false,
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
					}
				]
			],
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
							"[\"chunk1_s0_1\", {})"
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
	}
}
```

### Distinct on "shardKey", with filter: { "notShardKey" : { "$gte" : "1notShardKey_chunk1_s0_1" } }
### Expected results
`[ "chunk1_s0_0", "chunk1_s0_0_orphan", "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Distinct results
`[ "chunk1_s0_0", "chunk1_s0_0_orphan", "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "SHARDING_FILTER"
				},
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$gte" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "SHARDING_FILTER"
				},
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$gte" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		}
	}
}
```

### $group on shard key
### Pipeline
```json
[
	{
		"$match" : {
			
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
{  "_id" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_0_orphan" }
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_1_orphan" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s0_2_orphan" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_0_orphan" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_1_orphan" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk1_s1_2_orphan" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_0_orphan" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_1_orphan" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s0_2_orphan" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_0_orphan" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_1_orphan" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk2_s1_2_orphan" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_0_orphan" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_1_orphan" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s0_2_orphan" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_0_orphan" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_1_orphan" }
{  "_id" : "chunk3_s1_2" }
{  "_id" : "chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
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
	"distinct_scan_read_concern_available-rs1" : [
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
				"nss" : "test.distinct_scan_read_concern_available",
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
		"$match" : {
			"shardKey" : {
				"$gte" : "chunk1_s0_1"
			}
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
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_1_orphan" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s0_2_orphan" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_0_orphan" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_1_orphan" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk1_s1_2_orphan" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_0_orphan" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_1_orphan" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s0_2_orphan" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_0_orphan" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_1_orphan" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk2_s1_2_orphan" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_0_orphan" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_1_orphan" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s0_2_orphan" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_0_orphan" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_1_orphan" }
{  "_id" : "chunk3_s1_2" }
{  "_id" : "chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[\"chunk1_s0_1\", {})"
								]
							},
							"indexName" : "shardKey_1_notShardKey_1",
							"isFetching" : false,
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
						}
					]
				],
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
								"[\"chunk1_s0_1\", {})"
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
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[\"chunk1_s0_1\", {})"
								]
							},
							"indexName" : "shardKey_1_notShardKey_1",
							"isFetching" : false,
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
						}
					]
				],
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
								"[\"chunk1_s0_1\", {})"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"shardKey" : {
					"$gte" : "chunk1_s0_1"
				}
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
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
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
{  "_id" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_0_orphan" }
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_1_orphan" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s0_2_orphan" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_0_orphan" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_1_orphan" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk1_s1_2_orphan" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_0_orphan" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_1_orphan" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s0_2_orphan" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_0_orphan" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_1_orphan" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk2_s1_2_orphan" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_0_orphan" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_1_orphan" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s0_2_orphan" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_0_orphan" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_1_orphan" }
{  "_id" : "chunk3_s1_2" }
{  "_id" : "chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : {
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
					"notShardKey" : {
						"$gte" : "1notShardKey_chunk1_s0_1"
					}
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"distinct_scan_read_concern_available-rs1" : {
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
					"notShardKey" : {
						"$gte" : "1notShardKey_chunk1_s0_1"
					}
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"notShardKey" : {
					"$gte" : "1notShardKey_chunk1_s0_1"
				}
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

### distinct on non-shard key field
### Distinct on "notShardKey", with filter: { }
### Expected results
`[ "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_0_orphan", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_0_orphan", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

### Distinct on "notShardKey", with filter: { "shardKey" : { "$gte" : "chunk1_s0_1" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [
				[
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
								"[\"chunk1_s0_1\", {})"
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
			],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						],
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
						]
					},
					"indexName" : "shardKey_1_notShardKey_1",
					"isFetching" : false,
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
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [
				[
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
								"[\"chunk1_s0_1\", {})"
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
			],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						],
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
						]
					},
					"indexName" : "shardKey_1_notShardKey_1",
					"isFetching" : false,
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
				}
			]
		}
	}
}
```

### Distinct on "notShardKey", with filter: { "notShardKey" : { "$gte" : "1notShardKey_chunk1_s0_1" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_0_orphan", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", "notShardKey_chunk1_s0_0_orphan", "notShardKey_chunk1_s0_1_orphan", "notShardKey_chunk1_s0_2_orphan", "notShardKey_chunk1_s1_0_orphan", "notShardKey_chunk1_s1_1_orphan", "notShardKey_chunk1_s1_2_orphan", "notShardKey_chunk2_s0_0_orphan", "notShardKey_chunk2_s0_1_orphan", "notShardKey_chunk2_s0_2_orphan", "notShardKey_chunk2_s1_0_orphan", "notShardKey_chunk2_s1_1_orphan", "notShardKey_chunk2_s1_2_orphan", "notShardKey_chunk3_s0_0_orphan", "notShardKey_chunk3_s0_1_orphan", "notShardKey_chunk3_s0_2_orphan", "notShardKey_chunk3_s1_0_orphan", "notShardKey_chunk3_s1_1_orphan", "notShardKey_chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", {})"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", {})"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

### $group on non-shard key field
### Pipeline
```json
[
	{
		"$match" : {
			
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
{  "_id" : "notShardKey_chunk1_s0_0_orphan" }
{  "_id" : "notShardKey_chunk1_s0_1_orphan" }
{  "_id" : "notShardKey_chunk1_s0_2_orphan" }
{  "_id" : "notShardKey_chunk1_s1_0_orphan" }
{  "_id" : "notShardKey_chunk1_s1_1_orphan" }
{  "_id" : "notShardKey_chunk1_s1_2_orphan" }
{  "_id" : "notShardKey_chunk2_s0_0_orphan" }
{  "_id" : "notShardKey_chunk2_s0_1_orphan" }
{  "_id" : "notShardKey_chunk2_s0_2_orphan" }
{  "_id" : "notShardKey_chunk2_s1_0_orphan" }
{  "_id" : "notShardKey_chunk2_s1_1_orphan" }
{  "_id" : "notShardKey_chunk2_s1_2_orphan" }
{  "_id" : "notShardKey_chunk3_s0_0_orphan" }
{  "_id" : "notShardKey_chunk3_s0_1_orphan" }
{  "_id" : "notShardKey_chunk3_s0_2_orphan" }
{  "_id" : "notShardKey_chunk3_s1_0_orphan" }
{  "_id" : "notShardKey_chunk3_s1_1_orphan" }
{  "_id" : "notShardKey_chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
				"_id" : "$notShardKey"
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$match" : {
			"shardKey" : {
				"$gte" : "chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
{  "_id" : "notShardKey_chunk1_s0_1_orphan" }
{  "_id" : "notShardKey_chunk1_s0_2_orphan" }
{  "_id" : "notShardKey_chunk1_s1_0_orphan" }
{  "_id" : "notShardKey_chunk1_s1_1_orphan" }
{  "_id" : "notShardKey_chunk1_s1_2_orphan" }
{  "_id" : "notShardKey_chunk2_s0_0_orphan" }
{  "_id" : "notShardKey_chunk2_s0_1_orphan" }
{  "_id" : "notShardKey_chunk2_s0_2_orphan" }
{  "_id" : "notShardKey_chunk2_s1_0_orphan" }
{  "_id" : "notShardKey_chunk2_s1_1_orphan" }
{  "_id" : "notShardKey_chunk2_s1_2_orphan" }
{  "_id" : "notShardKey_chunk3_s0_0_orphan" }
{  "_id" : "notShardKey_chunk3_s0_1_orphan" }
{  "_id" : "notShardKey_chunk3_s0_2_orphan" }
{  "_id" : "notShardKey_chunk3_s1_0_orphan" }
{  "_id" : "notShardKey_chunk3_s1_1_orphan" }
{  "_id" : "notShardKey_chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
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
							"[\"chunk1_s0_1\", {})"
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
		],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[\"chunk1_s0_1\", {})"
					]
				},
				"indexName" : "shardKey_1_notShardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		]
	},
	"distinct_scan_read_concern_available-rs1" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
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
							"[\"chunk1_s0_1\", {})"
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
		],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[\"chunk1_s0_1\", {})"
					]
				},
				"indexName" : "shardKey_1_notShardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"shardKey" : {
					"$gte" : "chunk1_s0_1"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$notShardKey"
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
{  "_id" : "notShardKey_chunk1_s0_0_orphan" }
{  "_id" : "notShardKey_chunk1_s0_1_orphan" }
{  "_id" : "notShardKey_chunk1_s0_2_orphan" }
{  "_id" : "notShardKey_chunk1_s1_0_orphan" }
{  "_id" : "notShardKey_chunk1_s1_1_orphan" }
{  "_id" : "notShardKey_chunk1_s1_2_orphan" }
{  "_id" : "notShardKey_chunk2_s0_0_orphan" }
{  "_id" : "notShardKey_chunk2_s0_1_orphan" }
{  "_id" : "notShardKey_chunk2_s0_2_orphan" }
{  "_id" : "notShardKey_chunk2_s1_0_orphan" }
{  "_id" : "notShardKey_chunk2_s1_1_orphan" }
{  "_id" : "notShardKey_chunk2_s1_2_orphan" }
{  "_id" : "notShardKey_chunk3_s0_0_orphan" }
{  "_id" : "notShardKey_chunk3_s0_1_orphan" }
{  "_id" : "notShardKey_chunk3_s0_2_orphan" }
{  "_id" : "notShardKey_chunk3_s1_0_orphan" }
{  "_id" : "notShardKey_chunk3_s1_1_orphan" }
{  "_id" : "notShardKey_chunk3_s1_2_orphan" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[\"1notShardKey_chunk1_s0_1\", {})"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[\"1notShardKey_chunk1_s0_1\", {})"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"notShardKey" : {
					"$gte" : "1notShardKey_chunk1_s0_1"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$notShardKey"
			}
		}
	]
}
```

## 3. With readConcern 'majority' set via command options
### distinct on shard key
### Distinct on "shardKey", with filter: { }, and options: { "readConcern" : { "level" : "majority" } }
### Expected results
`[ "chunk1_s0_0", "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Distinct results
`[ "chunk1_s0_0", "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
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
		},
		"distinct_scan_read_concern_available-rs1" : {
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
	}
}
```

### Distinct on "shardKey", with filter: { "shardKey" : { "$gte" : "chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "majority" } }
### Expected results
`[ "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Distinct results
`[ "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [
				[
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
						"isFetching" : false,
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
					}
				]
			],
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
							"[\"chunk1_s0_1\", {})"
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
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [
				[
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[\"chunk1_s0_1\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
						"isFetching" : false,
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
					}
				]
			],
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
							"[\"chunk1_s0_1\", {})"
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
	}
}
```

### Distinct on "shardKey", with filter: { "notShardKey" : { "$gte" : "1notShardKey_chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "majority" } }
### Expected results
`[ "chunk1_s0_0", "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Distinct results
`[ "chunk1_s0_0", "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "SHARDING_FILTER"
				},
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$gte" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "SHARDING_FILTER"
				},
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$gte" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		}
	}
}
```

### $group on shard key
### Pipeline
```json
[
	{
		"$match" : {
			
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "majority" } }
```
### Results
```json
{  "_id" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
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
	"distinct_scan_read_concern_available-rs1" : [
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
				"nss" : "test.distinct_scan_read_concern_available",
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
		"$match" : {
			"shardKey" : {
				"$gte" : "chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "majority" } }
```
### Results
```json
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[\"chunk1_s0_1\", {})"
								]
							},
							"indexName" : "shardKey_1_notShardKey_1",
							"isFetching" : false,
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
						}
					]
				],
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
								"[\"chunk1_s0_1\", {})"
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
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[\"chunk1_s0_1\", {})"
								]
							},
							"indexName" : "shardKey_1_notShardKey_1",
							"isFetching" : false,
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
						}
					]
				],
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
								"[\"chunk1_s0_1\", {})"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"shardKey" : {
					"$gte" : "chunk1_s0_1"
				}
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
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "majority" } }
```
### Results
```json
{  "_id" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : {
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
					"notShardKey" : {
						"$gte" : "1notShardKey_chunk1_s0_1"
					}
				},
				"stage" : "COLLSCAN"
			}
		]
	},
	"distinct_scan_read_concern_available-rs1" : {
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
					"notShardKey" : {
						"$gte" : "1notShardKey_chunk1_s0_1"
					}
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"notShardKey" : {
					"$gte" : "1notShardKey_chunk1_s0_1"
				}
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

### distinct on non-shard key field
### Distinct on "notShardKey", with filter: { }, and options: { "readConcern" : { "level" : "majority" } }
### Expected results
`[ "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

### Distinct on "notShardKey", with filter: { "shardKey" : { "$gte" : "chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "majority" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [
				[
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
								"[\"chunk1_s0_1\", {})"
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
			],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						],
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
						]
					},
					"indexName" : "shardKey_1_notShardKey_1",
					"isFetching" : false,
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
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [
				[
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
								"[\"chunk1_s0_1\", {})"
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
			],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[MinKey, MaxKey]"
						],
						"shardKey" : [
							"[\"chunk1_s0_1\", {})"
						]
					},
					"indexName" : "shardKey_1_notShardKey_1",
					"isFetching" : false,
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
				}
			]
		}
	}
}
```

### Distinct on "notShardKey", with filter: { "notShardKey" : { "$gte" : "1notShardKey_chunk1_s0_1" } }, and options: { "readConcern" : { "level" : "majority" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_read_concern_available-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", {})"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_read_concern_available-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", {})"
						]
					},
					"indexName" : "notShardKey_1",
					"isFetching" : true,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

### $group on non-shard key field
### Pipeline
```json
[
	{
		"$match" : {
			
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "majority" } }
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
				"_id" : "$notShardKey"
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$match" : {
			"shardKey" : {
				"$gte" : "chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "majority" } }
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
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
							"[\"chunk1_s0_1\", {})"
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
		],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[\"chunk1_s0_1\", {})"
					]
				},
				"indexName" : "shardKey_1_notShardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		]
	},
	"distinct_scan_read_concern_available-rs1" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1
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
							"[\"chunk1_s0_1\", {})"
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
		],
		"winningPlan" : [
			{
				"stage" : "GROUP"
			},
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
				}
			},
			{
				"stage" : "SHARDING_FILTER"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[\"chunk1_s0_1\", {})"
					]
				},
				"indexName" : "shardKey_1_notShardKey_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		]
	},
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"shardKey" : {
					"$gte" : "chunk1_s0_1"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$notShardKey"
			}
		}
	]
}
```

### Pipeline
```json
[
	{
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey"
		}
	}
]
```
### Options
```json
{ "readConcern" : { "level" : "majority" } }
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_read_concern_available-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[\"1notShardKey_chunk1_s0_1\", {})"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_read_concern_available-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[\"1notShardKey_chunk1_s0_1\", {})"
							]
						},
						"indexName" : "notShardKey_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$notShardKey"
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
				"nss" : "test.distinct_scan_read_concern_available",
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
			"$match" : {
				"notShardKey" : {
					"$gte" : "1notShardKey_chunk1_s0_1"
				}
			}
		},
		{
			"$group" : {
				"_id" : "$notShardKey"
			}
		}
	]
}
```

