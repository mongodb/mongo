## 1. Only DISTINCT_SCAN candidates considered
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[MinKey, MaxKey]"
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
					],
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"d" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_1_d_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"d" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"d" : [ ]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"a" : 1,
			"b" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MaxKey, MinKey]"
								]
							},
							"indexName" : "a_1_b_-1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : -1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "a_-1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : -1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"a" : -1,
			"b" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$last" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[MinKey, MaxKey]"
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
					],
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"d" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_1_d_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"d" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"d" : [ ]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"a" : 1,
						"b" : 1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 7 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1,
								"c" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[MinKey, MaxKey]"
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
					],
					[
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"d" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_1_d_1",
							"isFetching" : true,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"d" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"d" : [ ]
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
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$c"
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
			"_id" : "$a",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"a" : -1,
						"b" : -1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 7 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1,
								"c" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[MinKey, MaxKey]"
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
					],
					[
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"d" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_1_d_1",
							"isFetching" : true,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"d" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"d" : [ ]
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
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$c"
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
			"_id" : "$a",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"a" : 1,
						"b" : -1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 7 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"direction" : "backward",
							"indexBounds" : {
								"a" : [
									"[MaxKey, MinKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_-1",
							"isFetching" : true,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : -1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ]
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
								"[MaxKey, MinKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_-1_b_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : -1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$c"
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
			"_id" : "$_id",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "accum" : 2 }
{  "_id" : 2,  "accum" : 3 }
{  "_id" : 3,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"_id" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "_id_",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : true,
						"keyPattern" : {
							"_id" : 1
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$_id",
					"accum" : "$b"
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
			"a" : 1,
			"b" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$last" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"a" : [
									"[MaxKey, MinKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_-1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : -1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MaxKey, MinKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_-1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : -1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"a" : -1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$last" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MaxKey, MinKey]"
								]
							},
							"indexName" : "a_1_b_-1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : -1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "a_-1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : -1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"_id" : "$d",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"d" : -1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 6 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"c" : 1,
							"d" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"c" : [
								"[MinKey, MaxKey]"
							],
							"d" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "d_1_c_-1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"c" : -1,
							"d" : 1
						},
						"multiKeyPaths" : {
							"c" : [ ],
							"d" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$d",
					"accum" : "$c"
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
			"d" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$d",
			"accum" : {
				"$first" : "$c"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 6 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"c" : 1,
							"d" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"c" : [
								"[MinKey, MaxKey]"
							],
							"d" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "d_1_c_-1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"c" : -1,
							"d" : 1
						},
						"multiKeyPaths" : {
							"c" : [ ],
							"d" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$d",
					"accum" : "$c"
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
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Options
```json
{ "hint" : "a_1_b_1" }
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Options
```json
{ "hint" : "a_1_b_1_c_1" }
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							],
							"c" : [
								"[MinKey, MaxKey]"
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
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Options
```json
{ "hint" : "a_1_b_1" }
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"a" : 1,
						"b" : 1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Options
```json
{ "hint" : "a_1_b_1" }
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 7 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$c"
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
			"_id" : "$a",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"a" : -1,
						"b" : -1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Options
```json
{ "hint" : "a_1_b_1" }
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 7 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$c"
				}
			}
		}
	]
}
```

### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 4 }
{  "_id" : 5 }
```
### Summarized explain
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
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
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ]
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

## 2. Both DISTINCT_SCAN and non-DISTINCT_SCAN candidates considered
### DISTINCT_SCAN selected
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : -1,
			"b" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$last" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[MinKey, MaxKey]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[MinKey, MaxKey]"
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
					],
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"a" : [
									"[MaxKey, MinKey]"
								],
								"b" : [
									"[MaxKey, MinKey]"
								],
								"d" : [
									"[MaxKey, MinKey]"
								]
							},
							"indexName" : "a_1_b_1_d_1",
							"isMultiKey" : true,
							"isPartial" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"d" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"d" : [
									"d"
								]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
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
			"a" : -1,
			"b" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"a" : [
									"[MaxKey, MinKey]"
								],
								"b" : [
									"[MaxKey, MinKey]"
								],
								"c" : [
									"[MaxKey, MinKey]"
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
					],
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "backward",
							"indexBounds" : {
								"a" : [
									"[MaxKey, MinKey]"
								],
								"b" : [
									"[MaxKey, MinKey]"
								],
								"d" : [
									"[MaxKey, MinKey]"
								]
							},
							"indexName" : "a_1_b_1_d_1",
							"isFetching" : false,
							"isMultiKey" : true,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"d" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"d" : [
									"d"
								]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "backward",
						"indexBounds" : {
							"a" : [
								"[MaxKey, MinKey]"
							],
							"b" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "a_1_b_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accum" : "$b"
				}
			}
		}
	]
}
```

### non-DISTINCT_SCAN selected, with hint
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : -1,
			"b" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$last" : "$b"
			}
		}
	}
]
```
### Options
```json
{ "hint" : "a_1_b_1_d_1" }
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : false,
				"a" : true,
				"b" : true
			}
		},
		{
			"direction" : "backward",
			"indexBounds" : {
				"a" : [
					"[MaxKey, MinKey]"
				],
				"b" : [
					"[MaxKey, MinKey]"
				],
				"d" : [
					"[MaxKey, MinKey]"
				]
			},
			"indexName" : "a_1_b_1_d_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1,
				"b" : 1,
				"d" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ],
				"b" : [ ],
				"d" : [
					"d"
				]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

### Pipeline
```json
[
	{
		"$sort" : {
			"a" : -1,
			"b" : -1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Options
```json
{ "hint" : { "$natural" : 1 } }
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 4 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"memLimit" : 104857600,
			"sortPattern" : {
				"a" : -1,
				"b" : -1
			},
			"stage" : "SORT",
			"type" : "simple"
		},
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false,
				"a" : true,
				"b" : true
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"stage" : "COLLSCAN"
		}
	]
}
```

## 3. DISTINCT_SCAN candidates choose index that covers projection, or smallest index if impossible
### No projection, pick smallest index
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 4 }
{  "_id" : 5 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
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
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ]
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

### Pick index that covers projection
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$a",
			"accumB" : {
				"$first" : "$b"
			},
			"accumC" : {
				"$first" : "$c"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accumB" : 2,  "accumC" : 3 }
{  "_id" : 5,  "accumB" : 4,  "accumC" : 7 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1,
							"c" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							],
							"c" : [
								"[MinKey, MaxKey]"
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
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accumB" : "$b",
					"accumC" : "$c"
				}
			}
		}
	]
}
```

### No index covers projection, pick smallest index
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$a",
			"accumB" : {
				"$first" : "$b"
			},
			"accumC" : {
				"$first" : "$c"
			},
			"accumD" : {
				"$first" : "$d"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accumB" : 2,  "accumC" : 3,  "accumD" : 4 }
{  "_id" : 5,  "accumB" : 4,  "accumC" : 7,  "accumD" : 5 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a",
					"accumB" : "$b",
					"accumC" : "$c",
					"accumD" : "$d"
				}
			}
		}
	]
}
```

### Multiplanning tie between DISTINCT_SCAN and IXSCAN
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gt" : 0
			}
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"a" : 1,
						"b" : 1
					},
					"output" : "$b"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "accum" : -1 }
{  "_id" : 2,  "accum" : -2 }
{  "_id" : 3,  "accum" : -3 }
{  "_id" : 4,  "accum" : -4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"(0.0, inf.0]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[inf.0, 0.0)"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_-1_b_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : -1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [ ],
							"b" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$a",
				"accum" : {
					"$top" : {
						"output" : "$b",
						"sortBy" : {
							"a" : 1,
							"b" : 1
						}
					}
				}
			}
		}
	]
}
```

### Prefer FETCH + filter + IXSCAN for more selective predicate on b
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gt" : 0
			},
			"b" : {
				"$gt" : -18
			}
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"a" : 1,
						"b" : 1
					},
					"output" : "$b"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "accum" : -1 }
{  "_id" : 10,  "accum" : -10 }
{  "_id" : 11,  "accum" : -11 }
{  "_id" : 12,  "accum" : -12 }
{  "_id" : 13,  "accum" : -13 }
{  "_id" : 14,  "accum" : -14 }
{  "_id" : 15,  "accum" : -15 }
{  "_id" : 16,  "accum" : -16 }
{  "_id" : 17,  "accum" : -17 }
{  "_id" : 2,  "accum" : -2 }
{  "_id" : 3,  "accum" : -3 }
{  "_id" : 4,  "accum" : -4 }
{  "_id" : 5,  "accum" : -5 }
{  "_id" : 6,  "accum" : -6 }
{  "_id" : 7,  "accum" : -7 }
{  "_id" : 8,  "accum" : -8 }
{  "_id" : 9,  "accum" : -9 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "PROJECTION_COVERED",
							"transformBy" : {
								"_id" : 0,
								"a" : 1,
								"b" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"(0.0, inf.0]"
								],
								"b" : [
									"(-18.0, inf.0]"
								]
							},
							"indexName" : "a_1_b_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ]
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
							"a" : 1,
							"b" : 1
						}
					},
					{
						"filter" : {
							"a" : {
								"$gt" : 0
							}
						},
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"b" : [
								"(-18.0, inf.0]"
							],
							"c" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "b_1_c_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"b" : 1,
							"c" : 1
						},
						"multiKeyPaths" : {
							"b" : [ ],
							"c" : [ ]
						},
						"stage" : "IXSCAN"
					}
				]
			}
		},
		{
			"$group" : {
				"_id" : "$a",
				"accum" : {
					"$top" : {
						"output" : "$b",
						"sortBy" : {
							"a" : 1,
							"b" : 1
						}
					}
				}
			}
		}
	]
}
```

## 4. No DISTINCT_SCAN candidates considered due to conflicting sort specs
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"b" : 1,
						"a" : 1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 7 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : 0,
					"a" : 1,
					"b" : 1,
					"c" : 1
				}
			},
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"a" : [
						"[MinKey, MaxKey]"
					],
					"b" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "a_1_b_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"a" : 1,
					"b" : 1
				},
				"multiKeyPaths" : {
					"a" : [ ],
					"b" : [ ]
				},
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : 0,
					"a" : 1,
					"b" : 1,
					"c" : 1
				}
			},
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"a" : [
						"[MinKey, MaxKey]"
					],
					"b" : [
						"[MinKey, MaxKey]"
					],
					"d" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "a_1_b_1_d_1",
				"isMultiKey" : true,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"a" : 1,
					"b" : 1,
					"d" : 1
				},
				"multiKeyPaths" : {
					"a" : [ ],
					"b" : [ ],
					"d" : [
						"d"
					]
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
				"a" : true,
				"b" : true,
				"c" : true
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey]"
				],
				"b" : [
					"[MinKey, MaxKey]"
				],
				"c" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1_b_1_c_1",
			"isMultiKey" : false,
			"isPartial" : false,
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
			"stage" : "IXSCAN"
		}
	]
}
```

### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$bottom" : {
					"sortBy" : {
						"a" : -1,
						"b" : -1
					},
					"output" : "$c"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 3 }
{  "_id" : 5,  "accum" : 7 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : 0,
					"a" : 1,
					"b" : 1,
					"c" : 1
				}
			},
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"a" : [
						"[MinKey, MaxKey]"
					],
					"b" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "a_1_b_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"a" : 1,
					"b" : 1
				},
				"multiKeyPaths" : {
					"a" : [ ],
					"b" : [ ]
				},
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : 0,
					"a" : 1,
					"b" : 1,
					"c" : 1
				}
			},
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"a" : [
						"[MinKey, MaxKey]"
					],
					"b" : [
						"[MinKey, MaxKey]"
					],
					"d" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "a_1_b_1_d_1",
				"isMultiKey" : true,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"a" : 1,
					"b" : 1,
					"d" : 1
				},
				"multiKeyPaths" : {
					"a" : [ ],
					"b" : [ ],
					"d" : [
						"d"
					]
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
				"a" : true,
				"b" : true,
				"c" : true
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey]"
				],
				"b" : [
					"[MinKey, MaxKey]"
				],
				"c" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1_b_1_c_1",
			"isMultiKey" : false,
			"isPartial" : false,
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
			"stage" : "IXSCAN"
		}
	]
}
```

## 5. No DISTINCT_SCAN candidates considered due to multikey index
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1,
			"b" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
{  "_id" : [ 1, 2, 3 ],  "accum" : 4 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : 0,
					"a" : 1,
					"b" : 1
				}
			},
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"a" : [
						"[MinKey, MaxKey]"
					],
					"b" : [
						"[MinKey, MaxKey]"
					],
					"c" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "a_1_b_1_c_1",
				"isMultiKey" : true,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"a" : 1,
					"b" : 1,
					"c" : 1
				},
				"multiKeyPaths" : {
					"a" : [
						"a"
					],
					"b" : [ ],
					"c" : [ ]
				},
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_SIMPLE",
				"transformBy" : {
					"_id" : 0,
					"a" : 1,
					"b" : 1
				}
			},
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"a" : [
						"[MinKey, MaxKey]"
					],
					"b" : [
						"[MinKey, MaxKey]"
					],
					"d" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "a_1_b_1_d_1",
				"isMultiKey" : true,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"a" : 1,
					"b" : 1,
					"d" : 1
				},
				"multiKeyPaths" : {
					"a" : [
						"a"
					],
					"b" : [ ],
					"d" : [
						"d"
					]
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
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey]"
				],
				"b" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "a_1_b_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1,
				"b" : 1
			},
			"multiKeyPaths" : {
				"a" : [
					"a"
				],
				"b" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 4 }
{  "_id" : 5 }
{  "_id" : [ 1, 2, 3 ] }
```
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"stage" : "COLLSCAN"
		}
	]
}
```

### No available indexes
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$a",
			"accum" : {
				"$top" : {
					"sortBy" : {
						"a" : 1,
						"b" : 1
					},
					"output" : "$b"
				}
			}
		}
	}
]
```
### Results
```json
{  "_id" : 4,  "accum" : 2 }
{  "_id" : 5,  "accum" : 4 }
{  "_id" : [ 1, 2, 3 ],  "accum" : 4 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"stage" : "COLLSCAN"
		}
	]
}
```

## 6. $group by non-multikey field with $first/$last on a multikey field
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$b",
			"accum" : {
				"$first" : "$a"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 2,  "accum" : 4 }
{  "_id" : 3,  "accum" : 4 }
{  "_id" : 4,  "accum" : 1 }
```
### Summarized explain
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1,
							"b" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"b" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "b_1_a_1",
						"isFetching" : false,
						"isMultiKey" : true,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"b" : 1
						},
						"multiKeyPaths" : {
							"a" : [
								"a"
							],
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$b",
					"accum" : "$a"
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
			"_id" : "$b",
			"accum" : {
				"$last" : "$a"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 2,  "accum" : 4 }
{  "_id" : 3,  "accum" : 4 }
{  "_id" : 4,  "accum" : [ 1, 2, 3 ] }
```
### Summarized explain
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "backward",
						"indexBounds" : {
							"b" : [
								"[MaxKey, MinKey]"
							]
						},
						"indexName" : "b_1",
						"isFetching" : true,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"b" : 1
						},
						"multiKeyPaths" : {
							"b" : [ ]
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$b",
					"accum" : "$a"
				}
			}
		}
	]
}
```

