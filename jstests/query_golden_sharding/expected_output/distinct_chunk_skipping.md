## 1. Unsharded environment uses DISTINCT_SCAN with embedded FETCH
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gte" : "shard0"
			},
			"c" : {
				"$eq" : 1
			}
		}
	},
	{
		"$group" : {
			"_id" : "$a"
		}
	}
]
```
### Results
```json
{  "_id" : "shard0_1" }
{  "_id" : "shard0_2" }
{  "_id" : "shard1_2" }
```
### Summarized explain
```json
{
	"distinct_chunk_skipping-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[1.0, 1.0]"
								]
							},
							"indexName" : "a_1_b_1_c_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
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
							"a" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[\"shard0\", {})"
							],
							"c" : [
								"[1.0, 1.0]"
							]
						},
						"indexName" : "a_1_c_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"c" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"c" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
				}
			}
		}
	]
}
```

## 2. Selective query uses DISTINCT_SCAN + shard filtering + embedded FETCH, but no chunk skipping
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gte" : "shard0"
			},
			"c" : {
				"$eq" : 1
			}
		}
	},
	{
		"$group" : {
			"_id" : "$a"
		}
	}
]
```
### Results
```json
{  "_id" : "shard0_1" }
{  "_id" : "shard0_2" }
{  "_id" : "shard1_2" }
```
### Summarized explain
```json
{
	"distinct_chunk_skipping-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[1.0, 1.0]"
								]
							},
							"indexName" : "a_1_b_1_c_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					]
				],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[\"shard0\", {})"
							],
							"c" : [
								"[1.0, 1.0]"
							]
						},
						"indexName" : "a_1_c_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"c" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"c" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
				}
			}
		}
	],
	"distinct_chunk_skipping-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[1.0, 1.0]"
								]
							},
							"indexName" : "a_1_b_1_c_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					]
				],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[\"shard0\", {})"
							],
							"c" : [
								"[1.0, 1.0]"
							]
						},
						"indexName" : "a_1_c_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"c" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"c" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
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
				"nss" : "test.distinct_chunk_skipping",
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
				"$and" : [
					{
						"a" : {
							"$gte" : "shard0"
						}
					},
					{
						"c" : {
							"$eq" : 1
						}
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$a"
			}
		}
	]
}
```

## 3. Non-selective query uses DISTINCT_SCAN + shard filtering + embedded FETCH + chunk skipping
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gte" : "shard0"
			},
			"c" : {
				"$lte" : 1
			}
		}
	},
	{
		"$group" : {
			"_id" : "$a"
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
```
### Summarized explain
```json
{
	"distinct_chunk_skipping-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"c" : [
									"[-inf.0, 1.0]"
								]
							},
							"indexName" : "a_1_c_1",
							"isFetching" : true,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"c" : [ ]
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
							"a" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[\"shard0\", {})"
							],
							"b" : [
								"[MinKey, MaxKey]"
							],
							"c" : [
								"[-inf.0, 1.0]"
							]
						},
						"indexName" : "a_1_b_1_c_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1,
							"c" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ],
							"c" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
				}
			}
		}
	],
	"distinct_chunk_skipping-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[-inf.0, 1.0]"
								]
							},
							"indexName" : "a_1_b_1_c_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					]
				],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[\"shard0\", {})"
							],
							"c" : [
								"[-inf.0, 1.0]"
							]
						},
						"indexName" : "a_1_c_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : true,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"c" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"c" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
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
				"nss" : "test.distinct_chunk_skipping",
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
				"$and" : [
					{
						"a" : {
							"$gte" : "shard0"
						}
					},
					{
						"c" : {
							"$lte" : 1
						}
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$a"
			}
		}
	]
}
```

## 4. No DISTINCT_SCAN on 'a', shard filtering + FETCH + filter
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gte" : "shard0"
			},
			"c" : {
				"$eq" : 1
			}
		}
	},
	{
		"$group" : {
			"_id" : "$a"
		}
	}
]
```
### Results
```json
{  "_id" : "shard0_1" }
{  "_id" : "shard0_2" }
{  "_id" : "shard1_2" }
```
### Summarized explain
```json
{
	"distinct_chunk_skipping-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[1.0, 1.0]"
								]
							},
							"indexName" : "a_1_b_1_c_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					],
					[
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"c" : [
									"[1.0, 1.0]"
								]
							},
							"indexName" : "a_1_c_1",
							"isFetching" : true,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"c" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					]
				],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"a" : 1
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"filter" : {
							"a" : {
								"$gte" : "shard0"
							}
						},
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"c" : [
								"[1.0, 1.0]"
							]
						},
						"indexName" : "c_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"c" : 1
						},
						"multiKeyPaths" : {
							"c" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$a"
			}
		}
	],
	"distinct_chunk_skipping-rs1" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[1.0, 1.0]"
								]
							},
							"indexName" : "a_1_b_1_c_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					],
					[
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"shard0\", {})"
								],
								"c" : [
									"[1.0, 1.0]"
								]
							},
							"indexName" : "a_1_c_1",
							"isFetching" : true,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : true,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"c" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"c" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					]
				],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"a" : 1
						}
					},
					{
						"stage" : "SHARDING_FILTER"
					},
					{
						"filter" : {
							"a" : {
								"$gte" : "shard0"
							}
						},
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"c" : [
								"[1.0, 1.0]"
							]
						},
						"indexName" : "c_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"c" : 1
						},
						"multiKeyPaths" : {
							"c" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$a"
			}
		}
	],
	"mergeType" : "router",
	"mergerPart" : [
		{
			"$mergeCursors" : {
				"allowPartialResults" : false,
				"compareWholeSortKey" : false,
				"nss" : "test.distinct_chunk_skipping",
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
				"$and" : [
					{
						"a" : {
							"$gte" : "shard0"
						}
					},
					{
						"c" : {
							"$eq" : 1
						}
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$a"
			}
		}
	]
}
```

