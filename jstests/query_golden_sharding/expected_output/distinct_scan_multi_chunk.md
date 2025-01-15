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

