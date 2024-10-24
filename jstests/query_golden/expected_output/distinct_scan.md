## 1. distinct() without index
### Distinct on "a", with filter: { }
### Expected results
`[ 1, 2, 3, 4, 7 ]`
### Distinct results
`[ 1, 2, 3, 4, 7 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"direction" : "forward",
			"filter" : {
				
			},
			"stage" : "COLLSCAN"
		}
	]
}
```

## 2. Aggregation without index
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 1 }
{  "_id" : 2 }
{  "_id" : 3 }
{  "_id" : 4 }
{  "_id" : 7 }
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

## 3. distinct() with index on 'a'
### Distinct on "a", with filter: { }
### Expected results
`[ 1, 2, 3, 4, 7 ]`
### Distinct results
`[ 1, 2, 3, 4, 7 ]`
### Summarized explain
```json
{
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
```

## 4. Aggregation on $group with _id field 'a' with index on 'a'
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 1 }
{  "_id" : 2 }
{  "_id" : 3 }
{  "_id" : 4 }
{  "_id" : 7 }
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

## 5. distinct() with multiple choices for index
### Distinct on "a", with filter: { }
### Expected results
`[ 1, 2, 3, 4, 7 ]`
### Distinct results
`[ 1, 2, 3, 4, 7 ]`
### Summarized explain
```json
{
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
```

## 6. Aggregation with multiple choices for index
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$a",
			"firstField" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "firstField" : 1 }
{  "_id" : 2,  "firstField" : 3 }
{  "_id" : 3,  "firstField" : 7 }
{  "_id" : 4,  "firstField" : 2 }
{  "_id" : 7,  "firstField" : 1 }
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
					"firstField" : "$b"
				}
			}
		}
	]
}
```

### $sort influences index selection
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1
		}
	},
	{
		"$group" : {
			"_id" : "$a",
			"firstField" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "firstField" : 1 }
{  "_id" : 2,  "firstField" : 3 }
{  "_id" : 3,  "firstField" : 7 }
{  "_id" : 4,  "firstField" : 2 }
{  "_id" : 7,  "firstField" : 1 }
```
### Summarized explain
```json
{
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [
					[
						{
							"stage" : "FETCH"
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
					"firstField" : "$b"
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
			"firstField" : {
				"$first" : "$b"
			}
		}
	}
]
```
### Results
```json
{  "_id" : 1,  "firstField" : 1 }
{  "_id" : 2,  "firstField" : 3 }
{  "_id" : 3,  "firstField" : 7 }
{  "_id" : 4,  "firstField" : 2 }
{  "_id" : 7,  "firstField" : 1 }
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
					"firstField" : "$b"
				}
			}
		}
	]
}
```

## 7. distinct() with filter on 'a' with available indexes
### Distinct on "a", with filter: { "a" : { "$lte" : 3 } }
### Expected results
`[ 1, 2, 3 ]`
### Distinct results
`[ 1, 2, 3 ]`
### Summarized explain
```json
{
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
						"[-inf.0, 3.0]"
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
				"a" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[-inf.0, 3.0]"
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
```

## 8. Aggregation with filter on 'a' with available indexes
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$lte" : 3
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
```
### Summarized explain
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
								"a" : 1
							}
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[-inf.0, 3.0]"
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
							"a" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[-inf.0, 3.0]"
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

## 9. distinct() with filter on 'b' with available indexes
### Distinct on "a", with filter: { "b" : 3 }
### Expected results
`[ 2, 4 ]`
### Distinct results
`[ 2, 4 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"b" : [
					"[3.0, 3.0]"
				]
			},
			"indexName" : "b_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"b" : 1
			},
			"multiKeyPaths" : {
				"b" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

## 10. Aggregation with filter on 'b' with available indexes
### Pipeline
```json
[
	{
		"$match" : {
			"b" : 3
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
{  "_id" : 2 }
{  "_id" : 4 }
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
						"stage" : "PROJECTION_SIMPLE",
						"transformBy" : {
							"_id" : 0,
							"a" : 1
						}
					},
					{
						"stage" : "FETCH"
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"b" : [
								"[3.0, 3.0]"
							]
						},
						"indexName" : "b_1",
						"isMultiKey" : false,
						"isPartial" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"b" : 1
						},
						"multiKeyPaths" : {
							"b" : [ ]
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
	]
}
```

