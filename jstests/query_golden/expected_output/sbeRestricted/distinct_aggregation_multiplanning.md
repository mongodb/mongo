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
	"queryShapeHash" : "384E008C9BC532E80DBF3FDE111DF54B498D0792E80FC880981635B390F61479",
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
	"queryShapeHash" : "1EADB848A8A4F2176E7108A973296021E3E88DDAE1660BBD86D4BEC6422ECCF7",
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
	"queryShapeHash" : "B5549857D794285E3DE51105C94E247249F8D81B9785D8E57F4F20C22AEA7869",
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
	"queryShapeHash" : "8AC00ED09C7D5F9F1893B1B9DEC4A1B5124EDD623B2D1497E1FC08BBA47F7600",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
	"queryShapeHash" : "9EB0A6CA2CDC2E3FC6075DC79DC2912FC1F0D23AF4CAACF2398C295A7707B46E",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
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
	"queryShapeHash" : "7E15D0059AE2B0032FC30DB276FA858AE5C35B41C43A4AE61D40D40FF0C3B3BF",
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
	"queryShapeHash" : "8BBFFC89CB210E8AF3AA0D0200C9E5410456D2A67C21AA95485BCC92A562A0CC",
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
	"queryShapeHash" : "F1F1191D8FCC612B2BBA57396B4ECC7FA2902715D3357A70726F11BD0D8F9336",
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
	"queryShapeHash" : "6A6153A8C351ACB48B003EC635BDD365AADD05ADB23E6ED90E225E723C38AEA9",
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
	"queryShapeHash" : "E1F938FFDEF35732CC1EE19EA64FD02E904479EAED5089FFC5912DDCD4B116C5",
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
	"queryShapeHash" : "361C760B158EFCA70CD3938C0496EEBF74EE07F64CD558880814532CED9589F8",
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
	"queryShapeHash" : "9786093C1ED1520418742C253A488AC98843164801041BDCCAE4DE663485062D",
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
	"queryShapeHash" : "384E008C9BC532E80DBF3FDE111DF54B498D0792E80FC880981635B390F61479",
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
	"queryShapeHash" : "384E008C9BC532E80DBF3FDE111DF54B498D0792E80FC880981635B390F61479",
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
	"queryShapeHash" : "E1F938FFDEF35732CC1EE19EA64FD02E904479EAED5089FFC5912DDCD4B116C5",
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
	"queryShapeHash" : "8AC00ED09C7D5F9F1893B1B9DEC4A1B5124EDD623B2D1497E1FC08BBA47F7600",
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
	"queryShapeHash" : "9EB0A6CA2CDC2E3FC6075DC79DC2912FC1F0D23AF4CAACF2398C295A7707B46E",
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
Execution Engine: classic
```json
{
	"queryShapeHash" : "E0EEEB8BB73D7E386CC4FC1FEB8AD8623F0F4ADFFB0777E38FD10541DA95A70E",
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
	"queryShapeHash" : "B5549857D794285E3DE51105C94E247249F8D81B9785D8E57F4F20C22AEA7869",
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
	"queryShapeHash" : "C5ED07AE22C7F18CCCCD5E9C556E3A8F328E9B7808D2BE16275CCFBE1AD347C3",
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
	"queryShapeHash" : "B5549857D794285E3DE51105C94E247249F8D81B9785D8E57F4F20C22AEA7869",
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
	"queryShapeHash" : "C5ED07AE22C7F18CCCCD5E9C556E3A8F328E9B7808D2BE16275CCFBE1AD347C3",
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
	"queryShapeHash" : "E0EEEB8BB73D7E386CC4FC1FEB8AD8623F0F4ADFFB0777E38FD10541DA95A70E",
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
	"queryShapeHash" : "B0975395FB8FA6672157D6B7980270F7E1C0F1918F9C8F7504DFBF2DCF88CC37",
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
	"queryShapeHash" : "4481AC8D4E52824BC07A5008C4EE90398C5104C45B4BD2A8F34B119914406166",
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

## 4. Rooted $or can only use a DISTINCT_SCAN when all predicates have mergeable bounds for a single index scan
### Rooted $or with one index bound uses DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$lte" : 5
					}
				},
				{
					"a" : {
						"$gt" : 8
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
	"queryShapeHash" : "21F6214B6C43364932E4F080EE15EC928B6E9D700B589DCCE93E7A6C4EC49AF9",
	"stages" : [
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
									"[-inf, 5.0]",
									"(8.0, inf]"
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
					],
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
									"[inf, 8.0)",
									"[5.0, -inf]"
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
					],
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
									"[-inf, 5.0]",
									"(8.0, inf]"
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
					],
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
									"[-inf, 5.0]",
									"(8.0, inf]"
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
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[-inf, 5.0]",
									"(8.0, inf]"
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
					],
					[
						{
							"stage" : "PROJECTION_DEFAULT",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"stage" : "OR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[-inf, 5.0]"
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
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"(8.0, inf]"
								]
							},
							"indexName" : "a_1",
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
						}
					],
					[
						{
							"stage" : "PROJECTION_DEFAULT",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"stage" : "OR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[5.0, -inf]"
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
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"(8.0, inf]"
								]
							},
							"indexName" : "a_1",
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
						}
					],
					[
						{
							"stage" : "PROJECTION_DEFAULT",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"stage" : "OR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[-inf, 5.0]"
								],
								"b" : [
									"[MaxKey, MinKey]"
								]
							},
							"indexName" : "a_1_b_-1",
							"isMultiKey" : false,
							"isPartial" : false,
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
							"stage" : "IXSCAN"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"(8.0, inf]"
								]
							},
							"indexName" : "a_1",
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
						}
					],
					[
						{
							"stage" : "PROJECTION_DEFAULT",
							"transformBy" : {
								"_id" : 0,
								"a" : 1
							}
						},
						{
							"stage" : "OR"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[-inf, 5.0]"
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
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"(8.0, inf]"
								]
							},
							"indexName" : "a_1",
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
								"[-inf, 5.0]",
								"(8.0, inf]"
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

### Rooted $or + $and on one field prevents use of DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$lte" : 5
					}
				},
				{
					"a" : {
						"$gt" : 6,
						"$lt" : 7
					}
				},
				{
					"a" : {
						"$gt" : 8
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
```
### Results
```json
{  "_id" : 4 }
{  "_id" : 5 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "37B43CA1C0FB317B870BD1F790EEE6BE0FAB2E3DB93AAAAA13D461B03C16CD3E",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"stage" : "OR"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[-inf, 5.0]",
					"(8.0, inf]"
				]
			},
			"indexName" : "a_1",
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
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(6.0, 7.0)"
				]
			},
			"indexName" : "a_1",
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
		}
	]
}
```

### Rooted $or on different fields prevents use of DISTINCT_SCAN
### Pipeline
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$gt" : 0
					}
				},
				{
					"b" : {
						"$lt" : 10
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
```
### Results
```json
{  "_id" : 4 }
{  "_id" : 5 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "13408003B588C1FFA53B8CC9AAFF93761264435C32A58746E3A15A8CE35F69C3",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"stage" : "OR"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[MinKey, MaxKey]"
				],
				"b" : [
					"[-inf, 10.0)"
				]
			},
			"indexName" : "b_1_a_1",
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
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(0.0, inf]"
				]
			},
			"indexName" : "a_1",
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
		}
	]
}
```

### Pipeline
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$gt" : 0
					},
					"b" : {
						"$lt" : 10
					}
				},
				{
					"a" : {
						"$lt" : 10
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
```
### Results
```json
{  "_id" : 4 }
{  "_id" : 5 }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "432A9158ED655AA2E6C4C6E8438340542521688D117A8153DB867FEF488650F5",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "GROUP"
		},
		{
			"stage" : "OR"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[-inf, 10.0)"
				]
			},
			"indexName" : "a_1",
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
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(0.0, inf]"
				],
				"b" : [
					"[-inf, 10.0)"
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
	]
}
```

### Multiplanning tie between DISTINCT_SCAN and IXSCAN favors DISTINCT_SCAN
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
	"queryShapeHash" : "53B0CD09B8404C8AEE54F9D7A391CD19538BFE2DEBFDDFC6C55D734963E24A08",
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
									"[inf, 0.0)"
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
								"(0.0, inf]"
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
	"queryShapeHash" : "847248D15DED36E13C8A7310ED7FB2EB4CE2B46DB3FB441A5B7AAAF9939B7B4D",
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
									"(0.0, inf]"
								],
								"b" : [
									"(-18.0, inf]"
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
								"(-18.0, inf]"
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
				"$willBeMerged" : false,
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

## 5. No DISTINCT_SCAN candidates considered due to conflicting sort specs
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
	"queryShapeHash" : "B290F1F0754AFA741AC27FE90C0C0701A45B515F7E10397C24A970CA04F4FF0B",
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
	"queryShapeHash" : "6A4A78B9197D6D627C43DB3CDD90F89856B5E1F07737480DE09A88D40D49E565",
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

## 6. No DISTINCT_SCAN candidates considered due to multikey index
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
	"queryShapeHash" : "384E008C9BC532E80DBF3FDE111DF54B498D0792E80FC880981635B390F61479",
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
Execution Engine: sbe
```json
{
	"queryShapeHash" : "E0EEEB8BB73D7E386CC4FC1FEB8AD8623F0F4ADFFB0777E38FD10541DA95A70E",
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
	"queryShapeHash" : "3BFF4D461425627D253B65EE45BE876532C7CEA15DE3A4A056CEFCE296E4B9F0",
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

## 7. $group by non-multikey field with $first/$last on a multikey field
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
{  "_id" : 4,  "accum" : 5 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "82FBF291BE13EF3DC272DF05D1A0DE562EBCF1607BD3F4C251E518BA93054F8A",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"direction" : "forward",
						"indexBounds" : {
							"b" : [
								"[MinKey, MaxKey]"
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
Execution Engine: classic
```json
{
	"queryShapeHash" : "42E75CEF190612636315E83DFA250B7519FFF856318644CC732D9828FCCEC506",
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

### Multiplanning tie between DISTINCT_SCANs favors fewest index keys
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
			"_id" : "$a"
		}
	}
]
```
### Results
```json
{  "_id" : 1 }
{  "_id" : 2 }
{  "_id" : 3 }
{  "_id" : 4 }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "406E789B98112863EB48B167CAC6254B38ACADA97DBE3808BF866D543142C4F6",
	"stages" : [
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
									"(0.0, inf]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[MinKey, MaxKey]"
								],
								"d" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_1_c_1_d_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1,
								"d" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ],
								"d" : [ ]
							},
							"stage" : "DISTINCT_SCAN"
						}
					],
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
									"(0.0, inf]"
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
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"(0.0, inf]"
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
					],
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
									"(0.0, inf]"
								],
								"b" : [
									"[MinKey, MaxKey]"
								],
								"c" : [
									"[MinKey, MaxKey]"
								],
								"d" : [
									"[MinKey, MaxKey]"
								],
								"e" : [
									"[MinKey, MaxKey]"
								]
							},
							"indexName" : "a_1_b_1_c_1_d_1_e_1",
							"isFetching" : false,
							"isMultiKey" : false,
							"isPartial" : false,
							"isShardFiltering" : false,
							"isSparse" : false,
							"isUnique" : false,
							"keyPattern" : {
								"a" : 1,
								"b" : 1,
								"c" : 1,
								"d" : 1,
								"e" : 1
							},
							"multiKeyPaths" : {
								"a" : [ ],
								"b" : [ ],
								"c" : [ ],
								"d" : [ ],
								"e" : [ ]
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
								"(0.0, inf]"
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

