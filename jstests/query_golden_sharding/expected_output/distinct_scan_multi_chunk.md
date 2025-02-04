## 1. distinct on shard key
### Distinct on "shardKey", with filter: { }
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
	"queryShapeHash" : "3E08037AA6B94982B95D1C3F310D7433C831031FBDB14536694772A401A17D74",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
		"distinct_scan_multi_chunk-rs1" : {
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

### Distinct on "shardKey", with filter: { "shardKey" : { "$eq" : "chunk1_s0_1" } }
### Expected results
`[ "chunk1_s0_1" ]`
### Distinct results
`[ "chunk1_s0_1" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SINGLE_SHARD"
		}
	],
	"queryShapeHash" : "A2E645EFCABA07A7713DE47195294FD5ED89805F90755BAC5ADCA9B0947C08DF",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
								"[\"chunk1_s0_1\", \"chunk1_s0_1\"]"
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
							"[\"chunk1_s0_1\", \"chunk1_s0_1\"]"
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

### Distinct on "shardKey", with filter: { "notShardKey" : { "$eq" : "1notShardKey_chunk1_s0_1" } }
### Expected results
`[ "chunk1_s0_1" ]`
### Distinct results
`[ "chunk1_s0_1" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"queryShapeHash" : "43D52AF789A031277DF7B982642BE63DBB84C9CE559EB19640726F46AA869583",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "SHARDING_FILTER"
				},
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$eq" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		},
		"distinct_scan_multi_chunk-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "SHARDING_FILTER"
				},
				{
					"direction" : "forward",
					"filter" : {
						"notShardKey" : {
							"$eq" : "1notShardKey_chunk1_s0_1"
						}
					},
					"stage" : "COLLSCAN"
				}
			]
		}
	}
}
```

### Distinct on "shardKey", with filter: { "shardKey" : { "$gte" : "chunk1_s0_1" } }
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
	"queryShapeHash" : "6AB45EB405ECDE6720DF6CAD435646D923D1F8FCE3A84612897860568A573EE0",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
		"distinct_scan_multi_chunk-rs1" : {
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
	"queryShapeHash" : "D437E367ECCB57701D4D444B3FF1FAB555B549A3B5A481475316B57F608844DE",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
		"distinct_scan_multi_chunk-rs1" : {
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

## 2. distinct on non-shard key field
### Distinct on "notShardKey", with filter: { }
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
	"queryShapeHash" : "76589B37FD0034DB3F63DE75126FA9683C91007FD7E3F90AFB08CA47547D2C9C",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
		"distinct_scan_multi_chunk-rs1" : {
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

### Distinct on "notShardKey", with filter: { "shardKey" : { "$eq" : "chunk1_s0_1" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_1" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_1" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SINGLE_SHARD"
		}
	],
	"queryShapeHash" : "11E9FD43BEA7007683708274AD27F956E40C807F879CFB1BB5CC41CAF86F58DA",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
			"rejectedPlans" : [
				[
					{
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"shardKey" : [
								"[\"chunk1_s0_1\", \"chunk1_s0_1\"]"
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
							"[\"chunk1_s0_1\", \"chunk1_s0_1\"]"
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

### Distinct on "notShardKey", with filter: { "notShardKey" : { "$eq" : "1notShardKey_chunk1_s0_1" } }
### Expected results
`[ "1notShardKey_chunk1_s0_1" ]`
### Distinct results
`[ "1notShardKey_chunk1_s0_1" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"queryShapeHash" : "F0FD51D26411738DE1ED291F14A2EA9CE2CEF16E084ED9785E571A512836BE69",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", \"1notShardKey_chunk1_s0_1\"]"
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
		"distinct_scan_multi_chunk-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[\"1notShardKey_chunk1_s0_1\", \"1notShardKey_chunk1_s0_1\"]"
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
	"queryShapeHash" : "C91CD944B07E20759F4D5348287AC796BD0B4E425C510B4B51BE6B7754452FA5",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
		"distinct_scan_multi_chunk-rs1" : {
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
	"queryShapeHash" : "7BCA6CC865058F39C39D3453DDD00D395E7B73C05C89AACDD69F0C8701EFC32D",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
		"distinct_scan_multi_chunk-rs1" : {
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

## 3. $group on a non-shard key field
### Pipeline
```json
[ { "$group" : { "_id" : "$notShardKey" } } ]
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
	"distinct_scan_multi_chunk-rs0" : [
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
	"distinct_scan_multi_chunk-rs1" : [
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
				"nss" : "test.distinct_scan_multi_chunk",
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
	"queryShapeHash" : "41D6076CE91480FEDA0B8CBC0403514B32425BE236EEBA8ED8C4435088424054",
	"shardsPart" : [
		{
			"$group" : {
				"_id" : "$notShardKey"
			}
		}
	]
}
```

### $group on a non-shard key field with $first/$last
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$first" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
					"_id" : "$notShardKey",
					"accum" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
					"_id" : "$notShardKey",
					"accum" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$first" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "D526F3E48F0AE31181590C35FA90DCAB8224F1F2A29CECF95D85025B33AA4D71",
	"shardsPart" : [
		{
			"$group" : {
				"_id" : "$notShardKey",
				"accum" : {
					"$first" : "$notShardKey"
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
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$first" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
					"_id" : "$notShardKey",
					"accum" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
					"_id" : "$notShardKey",
					"accum" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$first" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "7C9773F8C7BD4F3B581B9BC0B0FD907E76910635FA84BC315AD51A2D2CE11CE0",
	"shardsPart" : [
		{
			"$group" : {
				"_id" : "$notShardKey",
				"accum" : {
					"$first" : "$shardKey"
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
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$last" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
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
					"_id" : "$notShardKey",
					"accum" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
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
					"_id" : "$notShardKey",
					"accum" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$last" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "1D9A7FB5CF92475B04CBDA7BB9CBC8F81D8C39343891021433F252F5AB94A0BF",
	"shardsPart" : [
		{
			"$group" : {
				"_id" : "$notShardKey",
				"accum" : {
					"$last" : "$notShardKey"
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
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$last" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
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
					"_id" : "$notShardKey",
					"accum" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
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
					"_id" : "$notShardKey",
					"accum" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$last" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "B49952E5169C08D3CC5ACF2469FD5FD04D2885EB94754119AB1D891390C547EE",
	"shardsPart" : [
		{
			"$group" : {
				"_id" : "$notShardKey",
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	]
}
```

### $group on a non-shard key field with a preceding $match
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
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : {
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
	"distinct_scan_multi_chunk-rs1" : {
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
				"nss" : "test.distinct_scan_multi_chunk",
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
	"queryShapeHash" : "E14F75AAE6D3C1BFF4E041287502BA20D47E556668DA73200F4A8D3D59D50F5A",
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
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
	"distinct_scan_multi_chunk-rs1" : [
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
				"nss" : "test.distinct_scan_multi_chunk",
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
	"queryShapeHash" : "5789B22F27C1DFFE9FF3628C7A5B627E1481FCB23CD594F48C258156FAAA481E",
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

### $group on a non-shard key field with $first/$last a preceding $match
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
			"_id" : "$notShardKey",
			"accum" : {
				"$first" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : {
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
	"distinct_scan_multi_chunk-rs1" : {
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$first" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "1D978C7CE47AF02D59F282514EFBA5AB7D64E2F885B7AD0F2CAE2989A0D7F3A7",
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
				"_id" : "$notShardKey",
				"accum" : {
					"$first" : "$notShardKey"
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
		"$match" : {
			"shardKey" : {
				"$gte" : "chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$first" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1,
						"shardKey" : 1
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
					"notShardKey" : true,
					"shardKey" : true
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
	"distinct_scan_multi_chunk-rs1" : {
		"rejectedPlans" : [
			[
				{
					"stage" : "PROJECTION_SIMPLE",
					"transformBy" : {
						"_id" : 0,
						"notShardKey" : 1,
						"shardKey" : 1
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
					"notShardKey" : true,
					"shardKey" : true
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$first" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "C25420C4B1C67C657F7E0F38F3F6EAB622134BB6217F4CFE3822B2C41EC9DFE7",
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
				"_id" : "$notShardKey",
				"accum" : {
					"$first" : "$shardKey"
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
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$last" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"({}, \"1notShardKey_chunk1_s0_1\"]"
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
					"_id" : "$notShardKey",
					"accum" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"({}, \"1notShardKey_chunk1_s0_1\"]"
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
					"_id" : "$notShardKey",
					"accum" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$last" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "78E350AB796669698B14B659C4EBB5EC859A547DA50CD77D3B550C3D1E582350",
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
				"_id" : "$notShardKey",
				"accum" : {
					"$last" : "$notShardKey"
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
		"$match" : {
			"notShardKey" : {
				"$gte" : "1notShardKey_chunk1_s0_1"
			}
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$last" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"({}, \"1notShardKey_chunk1_s0_1\"]"
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
					"_id" : "$notShardKey",
					"accum" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"({}, \"1notShardKey_chunk1_s0_1\"]"
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
					"_id" : "$notShardKey",
					"accum" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$doingMerge" : true,
				"_id" : "$$ROOT._id",
				"accum" : {
					"$last" : "$$ROOT.accum"
				}
			}
		}
	],
	"queryShapeHash" : "C647799A58142640EFBD2B258811A0B5B850ECF002D27D837422C8CD6A2D089E",
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
				"_id" : "$notShardKey",
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	]
}
```

### $group on a non-shard key field with $first/$last and preceding $sort
### Pipeline
```json
[
	{
		"$sort" : {
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$first" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
	"distinct_scan_multi_chunk-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"notShardKey" : 1,
					"shardKey" : 1
				},
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$notShardKey",
				"accum" : {
					"$first" : "$notShardKey"
				}
			}
		}
	],
	"queryShapeHash" : "2805DB062A4586FC4A54ACE2C7C26C85396392D72B3A9F1007EACD616E3A3F89",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$project" : {
				"_id" : false,
				"notShardKey" : true
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
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$first" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true,
					"shardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
	"distinct_scan_multi_chunk-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true,
					"shardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"notShardKey" : 1,
					"shardKey" : 1
				},
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$notShardKey",
				"accum" : {
					"$first" : "$shardKey"
				}
			}
		}
	],
	"queryShapeHash" : "7B814C7C1F9A222ACE5CA59BF7EF6B5EE7322BA91944DB4B2974726F17E6F1BC",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$project" : {
				"_id" : false,
				"notShardKey" : true,
				"shardKey" : true
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
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$last" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "2notShardKey_chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "2notShardKey_chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "2notShardKey_chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "2notShardKey_chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "2notShardKey_chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "2notShardKey_chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "2notShardKey_chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "2notShardKey_chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "2notShardKey_chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "2notShardKey_chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "2notShardKey_chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "2notShardKey_chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "2notShardKey_chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "2notShardKey_chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "2notShardKey_chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "2notShardKey_chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "2notShardKey_chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "2notShardKey_chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
	"distinct_scan_multi_chunk-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"notShardKey" : 1,
					"shardKey" : 1
				},
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$notShardKey",
				"accum" : {
					"$last" : "$notShardKey"
				}
			}
		}
	],
	"queryShapeHash" : "D81AE22806B899E31B2E594EA88419E4D4A138E8FD95B78B66B666EA1922EF65",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$project" : {
				"_id" : false,
				"notShardKey" : true
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
			"notShardKey" : 1,
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$notShardKey",
			"accum" : {
				"$last" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "1notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "1notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "1notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "1notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "1notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "1notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "1notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "1notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "1notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "1notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "1notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "1notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "1notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "1notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "1notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "1notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "1notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "1notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "2notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "2notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "2notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "2notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "2notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "2notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "2notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "2notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "2notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "2notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "2notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "2notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "2notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "2notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "2notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "2notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "2notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "2notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
{  "_id" : "3notShardKey_chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "3notShardKey_chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "3notShardKey_chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "3notShardKey_chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "3notShardKey_chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "3notShardKey_chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "3notShardKey_chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "3notShardKey_chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "3notShardKey_chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "3notShardKey_chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "3notShardKey_chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "3notShardKey_chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "3notShardKey_chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "3notShardKey_chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "3notShardKey_chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "3notShardKey_chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "3notShardKey_chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "3notShardKey_chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true,
					"shardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
	"distinct_scan_multi_chunk-rs1" : {
		"rejectedPlans" : [ ],
		"winningPlan" : [
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : false,
					"notShardKey" : true,
					"shardKey" : true
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
					"notShardKey" : [
						"[MinKey, MaxKey]"
					],
					"shardKey" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "notShardKey_1_shardKey_1",
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"sort" : {
					"notShardKey" : 1,
					"shardKey" : 1
				},
				"tailableMode" : "normal"
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$notShardKey",
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	],
	"queryShapeHash" : "8AF395DE463AF073D422703077131CBBFA819B0517444DA5E081456E673E00A8",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$project" : {
				"_id" : false,
				"notShardKey" : true,
				"shardKey" : true
			}
		}
	]
}
```

## 4. $group on shard key with $top/$bottom
### sort by shard key, output shard key
### Pipeline
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
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "5F07638683238D8DC101347AC201D65CB865932937ADF7DB93B807E76249955C",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
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

### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"shardKey" : -1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : -1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : -1
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "E70DD5CA002009ED2F9DB267BD3A17BD4D09D2D0747942D8F174A0185249E056",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : -1
						}
					}
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
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$bottom" : {
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
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "2803CB698B2A4FBB38FD03CADC3B87EED812D28A2B7CF78ADB4502C70AAC71AE",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
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

### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"shardKey" : -1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : -1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : -1
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "D3F7A60442C0FBEF8122DA9DEABBCF0D42E222E7C0FCA7381959777F31B96B63",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"shardKey" : -1
						}
					}
				}
			}
		}
	]
}
```

### sort by shard key and another field, output shard key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"shardKey" : 1,
						"notShardKey" : 1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1,
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "FF36D9A7D66416B12BE7971000254B0E63CB73E8D898CA814AA96F97AABD676D",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
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
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"shardKey" : 1,
						"notShardKey" : 1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1,
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "13E2A2D1619930F4D954A2E616C921190A8C7470C6CE67F22D482C858D0A7C62",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
				}
			}
		}
	]
}
```

### sort by shard key and another field, output non-shard key field
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"shardKey" : 1,
						"notShardKey" : 1
					},
					"output" : "$notShardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1,
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "BEDCF013BB5367C37F261BF19FEE57640ABCA96F4767ED983AB66E309EE8685D",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
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
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"shardKey" : 1,
						"notShardKey" : 1
					},
					"output" : "$notShardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1,
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "8206D6BA91FE4D5680F2F393592494F4C5BF6E3F607894C92F7B6DB1FFE4DB1A",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1,
							"shardKey" : 1
						}
					}
				}
			}
		}
	]
}
```

### sort by non-shard key field, output shard key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"notShardKey" : 1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "90AE8A328542D80007D96B602F1966370A1FE6A3395A0B180B086D02255C98E9",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
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
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"notShardKey" : 1
					},
					"output" : "$shardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "D68448A8B1AD04D258FECDA8E44EFB3C2871AE502EE095A767428FB834736448",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$shardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
				}
			}
		}
	]
}
```

### sort by non-shard key field, output non-shard key field
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"notShardKey" : 1
					},
					"output" : "$notShardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "1E2C9158B1B301CFC113AE9AB389722D71F9DAB4F5392383824F6A278B2CBB44",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$top" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
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
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"notShardKey" : 1
					},
					"output" : "$notShardKey"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
						"stage" : "COLLSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "60DAC46FE5F99F3931B4178B8104C9D18AFF25B7E06DEC8F2446A685E526EAA9",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$bottom" : {
						"output" : "$notShardKey",
						"sortBy" : {
							"notShardKey" : 1
						}
					}
				}
			}
		}
	]
}
```

## 5. $group on shard key with $first/$last
### with preceding $sort on shard key
### Pipeline
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
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "7124EDE702422295D4AA8AF2AC99CB8DFB3E13FDB02223FED8C55E2386A13BCA",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : -1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
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
		"$sort" : {
			"shardKey" : 1
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
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "B08B5BE38981C61B18C61C0831F62D2B8EC1B31A26F1D5477245D5863EFDFEBA",
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
				"accum" : {
					"$first" : "$shardKey"
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
		"$sort" : {
			"shardKey" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$last" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"notShardKey" : [
									"[MinKey, MaxKey]"
								],
								"shardKey" : [
									"[MinKey, MaxKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "62A01372FA73C780549AEBB77546A4AFBC225B6684C1F5E65789284A0D3DD883",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"shardKey" : -1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
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
		"$sort" : {
			"shardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$last" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"stage" : "SORT_KEY_GENERATOR"
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"notShardKey" : [
									"[MaxKey, MinKey]"
								],
								"shardKey" : [
									"[MaxKey, MinKey]"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "BB300CA5A5B9B2EC92C87EF40194F06A729BBC6E024BF77382F7533B676D3DB4",
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
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	]
}
```

### with preceding $sort on shard key and another field
### Pipeline
```json
[
	{
		"$sort" : {
			"shardKey" : 1,
			"notShardKey" : 1
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
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "F673F86519CB56C7DF885FD9DD95312791D4AED2CDDB99144BED02479E57B166",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
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
		"$sort" : {
			"shardKey" : 1,
			"notShardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$last" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "6F2E5B6AA07F8E4E17C532B8EB785C32B6C76102FA637E104A844234D3C51C13",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
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
		"$sort" : {
			"shardKey" : 1,
			"notShardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$first" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "8199A1165EB52A74F63A0E14BB6AA321837D6BC83295C271FB3D64E141BC0D4A",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$notShardKey"
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
		"$sort" : {
			"shardKey" : 1,
			"notShardKey" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$last" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "CFD857DF6E9F02E673748FCA92FB07E1CFB8FE1C647FB71BAFCD1D35084DDC16",
	"shardsPart" : [
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$notShardKey"
				}
			}
		}
	]
}
```

### without preceding $sort, output shard key
### Pipeline
```json
[
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
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
					"_id" : "$shardKey",
					"accum" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
					"_id" : "$shardKey",
					"accum" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "93A5D1A7DCA2A7663975FD6AF7BB90FCAD7B3CF564ECD31CE96B32A78E5D4C8C",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$shardKey"
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
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$last" : "$shardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
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
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
					"_id" : "$shardKey",
					"accum" : "$shardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
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
						"direction" : "backward",
						"indexBounds" : {
							"shardKey" : [
								"[MaxKey, MinKey]"
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
					"_id" : "$shardKey",
					"accum" : "$shardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "78B2D5A49A22E7389F35323938117A780499ABBCB86EDE4ED3E0B823878AE6FB",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$shardKey"
				}
			}
		}
	]
}
```

### with preceding $sort and intervening $match
### Pipeline
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
			"l1" : {
				"$first" : "$shardKey"
			},
			"l2" : {
				"$first" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "l1" : "chunk1_s0_0",  "l2" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "l1" : "chunk1_s0_1",  "l2" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "l1" : "chunk1_s0_2",  "l2" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "l1" : "chunk1_s1_0",  "l2" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "l1" : "chunk1_s1_1",  "l2" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "l1" : "chunk1_s1_2",  "l2" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "l1" : "chunk2_s0_0",  "l2" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "l1" : "chunk2_s0_1",  "l2" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "l1" : "chunk2_s0_2",  "l2" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "l1" : "chunk2_s1_0",  "l2" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "l1" : "chunk2_s1_1",  "l2" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "l1" : "chunk2_s1_2",  "l2" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "l1" : "chunk3_s0_0",  "l2" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "l1" : "chunk3_s0_1",  "l2" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "l1" : "chunk3_s0_2",  "l2" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "l1" : "chunk3_s1_0",  "l2" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "l1" : "chunk3_s1_1",  "l2" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "l1" : "chunk3_s1_2",  "l2" : "1notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
						},
						{
							"stage" : "PROJECTION_SIMPLE",
							"transformBy" : {
								"_id" : 0,
								"notShardKey" : 1,
								"shardKey" : 1
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
									"(\"chunk1_s0\", {})"
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
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"(\"chunk1_s0\", {})"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"l1" : {
					"$first" : "$shardKey"
				},
				"l2" : {
					"$first" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
						},
						{
							"stage" : "PROJECTION_SIMPLE",
							"transformBy" : {
								"_id" : 0,
								"notShardKey" : 1,
								"shardKey" : 1
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
									"(\"chunk1_s0\", {})"
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
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"(\"chunk1_s0\", {})"
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"l1" : {
					"$first" : "$shardKey"
				},
				"l2" : {
					"$first" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "2378EB067EF041608B21915FF68CC21C92818AAE311CCEA873C02C02FB13D396",
	"shardsPart" : [
		{
			"$match" : {
				"shardKey" : {
					"$gt" : "chunk1_s0"
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"l1" : {
					"$first" : "$shardKey"
				},
				"l2" : {
					"$first" : "$notShardKey"
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
			"l1" : {
				"$last" : "$shardKey"
			},
			"l2" : {
				"$last" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "l1" : "chunk1_s0_0",  "l2" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "l1" : "chunk1_s0_1",  "l2" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "l1" : "chunk1_s0_2",  "l2" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "l1" : "chunk1_s1_0",  "l2" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "l1" : "chunk1_s1_1",  "l2" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "l1" : "chunk1_s1_2",  "l2" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "l1" : "chunk2_s0_0",  "l2" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "l1" : "chunk2_s0_1",  "l2" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "l1" : "chunk2_s0_2",  "l2" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "l1" : "chunk2_s1_0",  "l2" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "l1" : "chunk2_s1_1",  "l2" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "l1" : "chunk2_s1_2",  "l2" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "l1" : "chunk3_s0_0",  "l2" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "l1" : "chunk3_s0_1",  "l2" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "l1" : "chunk3_s0_2",  "l2" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "l1" : "chunk3_s1_0",  "l2" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "l1" : "chunk3_s1_1",  "l2" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "l1" : "chunk3_s1_2",  "l2" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
						},
						{
							"stage" : "PROJECTION_SIMPLE",
							"transformBy" : {
								"_id" : 0,
								"notShardKey" : 1,
								"shardKey" : 1
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
									"(\"chunk1_s0\", {})"
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
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"l1" : {
					"$last" : "$shardKey"
				},
				"l2" : {
					"$last" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
						},
						{
							"stage" : "PROJECTION_SIMPLE",
							"transformBy" : {
								"_id" : 0,
								"notShardKey" : 1,
								"shardKey" : 1
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
									"(\"chunk1_s0\", {})"
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
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
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
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"l1" : {
					"$last" : "$shardKey"
				},
				"l2" : {
					"$last" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "A0D64445DE8B50BC8EC335B3C0063A02D0C2A9B22F8484230FBB846960F14E3A",
	"shardsPart" : [
		{
			"$match" : {
				"shardKey" : {
					"$gt" : "chunk1_s0"
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"l1" : {
					"$last" : "$shardKey"
				},
				"l2" : {
					"$last" : "$notShardKey"
				}
			}
		}
	]
}
```

### without preceding $sort, output non-shard key field
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$first" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "1notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "1notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "1notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "1notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "1notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "1notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "1notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "1notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "1notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "1notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "1notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "1notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "1notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "1notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "1notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "1notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "1notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "1notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
								"[MinKey, MaxKey]"
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
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$shardKey",
					"accum" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
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
								"[MinKey, MaxKey]"
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
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$shardKey",
					"accum" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "70A4727A901E80E03246A8C7E6B0CC4A2225AFE7BA4B911C44FC89EDED220F90",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$first" : "$notShardKey"
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
		"$group" : {
			"_id" : "$shardKey",
			"accum" : {
				"$last" : "$notShardKey"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "accum" : "3notShardKey_chunk1_s0_0" }
{  "_id" : "chunk1_s0_1",  "accum" : "3notShardKey_chunk1_s0_1" }
{  "_id" : "chunk1_s0_2",  "accum" : "3notShardKey_chunk1_s0_2" }
{  "_id" : "chunk1_s1_0",  "accum" : "3notShardKey_chunk1_s1_0" }
{  "_id" : "chunk1_s1_1",  "accum" : "3notShardKey_chunk1_s1_1" }
{  "_id" : "chunk1_s1_2",  "accum" : "3notShardKey_chunk1_s1_2" }
{  "_id" : "chunk2_s0_0",  "accum" : "3notShardKey_chunk2_s0_0" }
{  "_id" : "chunk2_s0_1",  "accum" : "3notShardKey_chunk2_s0_1" }
{  "_id" : "chunk2_s0_2",  "accum" : "3notShardKey_chunk2_s0_2" }
{  "_id" : "chunk2_s1_0",  "accum" : "3notShardKey_chunk2_s1_0" }
{  "_id" : "chunk2_s1_1",  "accum" : "3notShardKey_chunk2_s1_1" }
{  "_id" : "chunk2_s1_2",  "accum" : "3notShardKey_chunk2_s1_2" }
{  "_id" : "chunk3_s0_0",  "accum" : "3notShardKey_chunk3_s0_0" }
{  "_id" : "chunk3_s0_1",  "accum" : "3notShardKey_chunk3_s0_1" }
{  "_id" : "chunk3_s0_2",  "accum" : "3notShardKey_chunk3_s0_2" }
{  "_id" : "chunk3_s1_0",  "accum" : "3notShardKey_chunk3_s1_0" }
{  "_id" : "chunk3_s1_1",  "accum" : "3notShardKey_chunk3_s1_1" }
{  "_id" : "chunk3_s1_2",  "accum" : "3notShardKey_chunk3_s1_2" }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$shardKey",
					"accum" : "$notShardKey"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"notShardKey" : 1,
							"shardKey" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"notShardKey" : [
								"[MaxKey, MinKey]"
							],
							"shardKey" : [
								"[MaxKey, MinKey]"
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
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$shardKey",
					"accum" : "$notShardKey"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "B898E74AFF183BDEAB7B15A77737DE426E623C0BDF502748A491605625BBB129",
	"shardsPart" : [
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"accum" : {
					"$last" : "$notShardKey"
				}
			}
		}
	]
}
```

### with preceding $sort and intervening $match, output non-shard key field
### Pipeline
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
				"$first" : "$$ROOT"
			}
		}
	}
]
```
### Results
```json
{  "_id" : "chunk1_s0_0",  "r" : {  "_id" : 0,  "notShardKey" : "1notShardKey_chunk1_s0_0",  "shardKey" : "chunk1_s0_0" } }
{  "_id" : "chunk1_s0_1",  "r" : {  "_id" : 3,  "notShardKey" : "1notShardKey_chunk1_s0_1",  "shardKey" : "chunk1_s0_1" } }
{  "_id" : "chunk1_s0_2",  "r" : {  "_id" : 6,  "notShardKey" : "1notShardKey_chunk1_s0_2",  "shardKey" : "chunk1_s0_2" } }
{  "_id" : "chunk1_s1_0",  "r" : {  "_id" : 27,  "notShardKey" : "1notShardKey_chunk1_s1_0",  "shardKey" : "chunk1_s1_0" } }
{  "_id" : "chunk1_s1_1",  "r" : {  "_id" : 30,  "notShardKey" : "1notShardKey_chunk1_s1_1",  "shardKey" : "chunk1_s1_1" } }
{  "_id" : "chunk1_s1_2",  "r" : {  "_id" : 33,  "notShardKey" : "1notShardKey_chunk1_s1_2",  "shardKey" : "chunk1_s1_2" } }
{  "_id" : "chunk2_s0_0",  "r" : {  "_id" : 9,  "notShardKey" : "1notShardKey_chunk2_s0_0",  "shardKey" : "chunk2_s0_0" } }
{  "_id" : "chunk2_s0_1",  "r" : {  "_id" : 12,  "notShardKey" : "1notShardKey_chunk2_s0_1",  "shardKey" : "chunk2_s0_1" } }
{  "_id" : "chunk2_s0_2",  "r" : {  "_id" : 15,  "notShardKey" : "1notShardKey_chunk2_s0_2",  "shardKey" : "chunk2_s0_2" } }
{  "_id" : "chunk2_s1_0",  "r" : {  "_id" : 36,  "notShardKey" : "1notShardKey_chunk2_s1_0",  "shardKey" : "chunk2_s1_0" } }
{  "_id" : "chunk2_s1_1",  "r" : {  "_id" : 39,  "notShardKey" : "1notShardKey_chunk2_s1_1",  "shardKey" : "chunk2_s1_1" } }
{  "_id" : "chunk2_s1_2",  "r" : {  "_id" : 42,  "notShardKey" : "1notShardKey_chunk2_s1_2",  "shardKey" : "chunk2_s1_2" } }
{  "_id" : "chunk3_s0_0",  "r" : {  "_id" : 18,  "notShardKey" : "1notShardKey_chunk3_s0_0",  "shardKey" : "chunk3_s0_0" } }
{  "_id" : "chunk3_s0_1",  "r" : {  "_id" : 21,  "notShardKey" : "1notShardKey_chunk3_s0_1",  "shardKey" : "chunk3_s0_1" } }
{  "_id" : "chunk3_s0_2",  "r" : {  "_id" : 24,  "notShardKey" : "1notShardKey_chunk3_s0_2",  "shardKey" : "chunk3_s0_2" } }
{  "_id" : "chunk3_s1_0",  "r" : {  "_id" : 45,  "notShardKey" : "1notShardKey_chunk3_s1_0",  "shardKey" : "chunk3_s1_0" } }
{  "_id" : "chunk3_s1_1",  "r" : {  "_id" : 48,  "notShardKey" : "1notShardKey_chunk3_s1_1",  "shardKey" : "chunk3_s1_1" } }
{  "_id" : "chunk3_s1_2",  "r" : {  "_id" : 51,  "notShardKey" : "1notShardKey_chunk3_s1_2",  "shardKey" : "chunk3_s1_2" } }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
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
									"(\"chunk1_s0\", {})"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"(\"chunk1_s0\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
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
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"r" : {
					"$first" : "$$ROOT"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
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
									"(\"chunk1_s0\", {})"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"(\"chunk1_s0\", {})"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
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
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"r" : {
					"$first" : "$$ROOT"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "F107F47CC9A005147AA53BCCF4CDD151C52D9CC5F6E477CB3055559EA43E34CE",
	"shardsPart" : [
		{
			"$match" : {
				"shardKey" : {
					"$gt" : "chunk1_s0"
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"r" : {
					"$first" : "$$ROOT"
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
### Results
```json
{  "_id" : "chunk1_s0_0",  "r" : {  "_id" : 2,  "notShardKey" : "3notShardKey_chunk1_s0_0",  "shardKey" : "chunk1_s0_0" } }
{  "_id" : "chunk1_s0_1",  "r" : {  "_id" : 5,  "notShardKey" : "3notShardKey_chunk1_s0_1",  "shardKey" : "chunk1_s0_1" } }
{  "_id" : "chunk1_s0_2",  "r" : {  "_id" : 8,  "notShardKey" : "3notShardKey_chunk1_s0_2",  "shardKey" : "chunk1_s0_2" } }
{  "_id" : "chunk1_s1_0",  "r" : {  "_id" : 29,  "notShardKey" : "3notShardKey_chunk1_s1_0",  "shardKey" : "chunk1_s1_0" } }
{  "_id" : "chunk1_s1_1",  "r" : {  "_id" : 32,  "notShardKey" : "3notShardKey_chunk1_s1_1",  "shardKey" : "chunk1_s1_1" } }
{  "_id" : "chunk1_s1_2",  "r" : {  "_id" : 35,  "notShardKey" : "3notShardKey_chunk1_s1_2",  "shardKey" : "chunk1_s1_2" } }
{  "_id" : "chunk2_s0_0",  "r" : {  "_id" : 11,  "notShardKey" : "3notShardKey_chunk2_s0_0",  "shardKey" : "chunk2_s0_0" } }
{  "_id" : "chunk2_s0_1",  "r" : {  "_id" : 14,  "notShardKey" : "3notShardKey_chunk2_s0_1",  "shardKey" : "chunk2_s0_1" } }
{  "_id" : "chunk2_s0_2",  "r" : {  "_id" : 17,  "notShardKey" : "3notShardKey_chunk2_s0_2",  "shardKey" : "chunk2_s0_2" } }
{  "_id" : "chunk2_s1_0",  "r" : {  "_id" : 38,  "notShardKey" : "3notShardKey_chunk2_s1_0",  "shardKey" : "chunk2_s1_0" } }
{  "_id" : "chunk2_s1_1",  "r" : {  "_id" : 41,  "notShardKey" : "3notShardKey_chunk2_s1_1",  "shardKey" : "chunk2_s1_1" } }
{  "_id" : "chunk2_s1_2",  "r" : {  "_id" : 44,  "notShardKey" : "3notShardKey_chunk2_s1_2",  "shardKey" : "chunk2_s1_2" } }
{  "_id" : "chunk3_s0_0",  "r" : {  "_id" : 20,  "notShardKey" : "3notShardKey_chunk3_s0_0",  "shardKey" : "chunk3_s0_0" } }
{  "_id" : "chunk3_s0_1",  "r" : {  "_id" : 23,  "notShardKey" : "3notShardKey_chunk3_s0_1",  "shardKey" : "chunk3_s0_1" } }
{  "_id" : "chunk3_s0_2",  "r" : {  "_id" : 26,  "notShardKey" : "3notShardKey_chunk3_s0_2",  "shardKey" : "chunk3_s0_2" } }
{  "_id" : "chunk3_s1_0",  "r" : {  "_id" : 47,  "notShardKey" : "3notShardKey_chunk3_s1_0",  "shardKey" : "chunk3_s1_0" } }
{  "_id" : "chunk3_s1_1",  "r" : {  "_id" : 50,  "notShardKey" : "3notShardKey_chunk3_s1_1",  "shardKey" : "chunk3_s1_1" } }
{  "_id" : "chunk3_s1_2",  "r" : {  "_id" : 53,  "notShardKey" : "3notShardKey_chunk3_s1_2",  "shardKey" : "chunk3_s1_2" } }
```
### Summarized explain
```json
{
	"distinct_scan_multi_chunk-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
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
									"(\"chunk1_s0\", {})"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
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
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"r" : {
					"$last" : "$$ROOT"
				}
			}
		}
	],
	"distinct_scan_multi_chunk-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"memLimit" : 104857600,
							"sortPattern" : {
								"notShardKey" : 1,
								"shardKey" : 1
							},
							"stage" : "SORT",
							"type" : "simple"
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
									"(\"chunk1_s0\", {})"
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
						"stage" : "SORT_KEY_GENERATOR"
					},
					{
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
					}
				]
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"r" : {
					"$last" : "$$ROOT"
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
				"nss" : "test.distinct_scan_multi_chunk",
				"recordRemoteOpWaitTime" : false,
				"requestQueryStatsFromRemotes" : false,
				"tailableMode" : "normal"
			}
		}
	],
	"queryShapeHash" : "6296BF1C8BAA694AFA0FC369C9332341DFA7C4684065F7DE6E6D74B91377C294",
	"shardsPart" : [
		{
			"$match" : {
				"shardKey" : {
					"$gt" : "chunk1_s0"
				}
			}
		},
		{
			"$sort" : {
				"sortKey" : {
					"notShardKey" : 1,
					"shardKey" : 1
				}
			}
		},
		{
			"$group" : {
				"$willBeMerged" : false,
				"_id" : "$shardKey",
				"r" : {
					"$last" : "$$ROOT"
				}
			}
		}
	]
}
```

## 6. distinct on multikey field
### Distinct on "notShardKey", with filter: { }
### Expected results
`[ 1, "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", 2, "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", 3, "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2", 4 ]`
### Distinct results
`[ 1, 2, 3, 4, "1notShardKey_chunk1_s0_0", "1notShardKey_chunk1_s0_1", "1notShardKey_chunk1_s0_2", "1notShardKey_chunk1_s1_0", "1notShardKey_chunk1_s1_1", "1notShardKey_chunk1_s1_2", "1notShardKey_chunk2_s0_0", "1notShardKey_chunk2_s0_1", "1notShardKey_chunk2_s0_2", "1notShardKey_chunk2_s1_0", "1notShardKey_chunk2_s1_1", "1notShardKey_chunk2_s1_2", "1notShardKey_chunk3_s0_0", "1notShardKey_chunk3_s0_1", "1notShardKey_chunk3_s0_2", "1notShardKey_chunk3_s1_0", "1notShardKey_chunk3_s1_1", "1notShardKey_chunk3_s1_2", "2notShardKey_chunk1_s0_0", "2notShardKey_chunk1_s0_1", "2notShardKey_chunk1_s0_2", "2notShardKey_chunk1_s1_0", "2notShardKey_chunk1_s1_1", "2notShardKey_chunk1_s1_2", "2notShardKey_chunk2_s0_0", "2notShardKey_chunk2_s0_1", "2notShardKey_chunk2_s0_2", "2notShardKey_chunk2_s1_0", "2notShardKey_chunk2_s1_1", "2notShardKey_chunk2_s1_2", "2notShardKey_chunk3_s0_0", "2notShardKey_chunk3_s0_1", "2notShardKey_chunk3_s0_2", "2notShardKey_chunk3_s1_0", "2notShardKey_chunk3_s1_1", "2notShardKey_chunk3_s1_2", "3notShardKey_chunk1_s0_0", "3notShardKey_chunk1_s0_1", "3notShardKey_chunk1_s0_2", "3notShardKey_chunk1_s1_0", "3notShardKey_chunk1_s1_1", "3notShardKey_chunk1_s1_2", "3notShardKey_chunk2_s0_0", "3notShardKey_chunk2_s0_1", "3notShardKey_chunk2_s0_2", "3notShardKey_chunk2_s1_0", "3notShardKey_chunk2_s1_1", "3notShardKey_chunk2_s1_2", "3notShardKey_chunk3_s0_0", "3notShardKey_chunk3_s0_1", "3notShardKey_chunk3_s0_2", "3notShardKey_chunk3_s1_0", "3notShardKey_chunk3_s1_1", "3notShardKey_chunk3_s1_2" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"queryShapeHash" : "76589B37FD0034DB3F63DE75126FA9683C91007FD7E3F90AFB08CA47547D2C9C",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
					"isMultiKey" : true,
					"isPartial" : false,
					"isShardFiltering" : true,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [
							"notShardKey"
						]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_multi_chunk-rs1" : {
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

### Distinct on "notShardKey", with filter: { "shardKey" : null }
### Expected results
`[ 1, 2, 3, 4 ]`
### Distinct results
`[ 1, 2, 3, 4 ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SINGLE_SHARD"
		}
	],
	"queryShapeHash" : "D09D4099F96FD44670F88482048901DC90312693A0FD1BAC8A9149A0F1D7EFDE",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
			"rejectedPlans" : [
				[
					{
						"filter" : {
							"shardKey" : {
								"$eq" : null
							}
						},
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[MinKey, MaxKey]"
							],
							"shardKey" : [
								"[null, null]"
							]
						},
						"indexName" : "shardKey_1_notShardKey_1",
						"isMultiKey" : true,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1,
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [
								"notShardKey"
							],
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			],
			"winningPlan" : [
				{
					"filter" : {
						"shardKey" : {
							"$eq" : null
						}
					},
					"stage" : "FETCH"
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"shardKey" : [
							"[null, null]"
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
}
```

### Distinct on "notShardKey", with filter: { "notShardKey" : 3 }
### Expected results
`[ 1, 2, 3, 4 ]`
### Distinct results
`[ 1, 2, 3, 4 ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"queryShapeHash" : "3E1D797E43038CA8E7628D34718426F514D9B987522E955DD91EA31987B2C949",
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
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
							"notShardKey" : [
								"[3.0, 3.0]"
							],
							"shardKey" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "notShardKey_1_shardKey_1",
						"isMultiKey" : true,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"notShardKey" : 1,
							"shardKey" : 1
						},
						"multiKeyPaths" : {
							"notShardKey" : [
								"notShardKey"
							],
							"shardKey" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			],
			"winningPlan" : [
				{
					"stage" : "SHARDING_FILTER"
				},
				{
					"stage" : "FETCH"
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"notShardKey" : [
							"[3.0, 3.0]"
						]
					},
					"indexName" : "notShardKey_1",
					"isMultiKey" : true,
					"isPartial" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"notShardKey" : 1
					},
					"multiKeyPaths" : {
						"notShardKey" : [
							"notShardKey"
						]
					},
					"stage" : "IXSCAN"
				}
			]
		},
		"distinct_scan_multi_chunk-rs1" : {
			"rejectedPlans" : [
				[
					{
						"direction" : "forward",
						"indexBounds" : {
							"notShardKey" : [
								"[3.0, 3.0]"
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
							"[3.0, 3.0]"
						],
						"shardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "notShardKey_1_shardKey_1",
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

