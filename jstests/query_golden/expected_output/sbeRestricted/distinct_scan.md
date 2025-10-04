## 1. distinct() without index
### Distinct on "a", with filter: { }
### Distinct results
`[ 1, 2, 3, 4, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "C1166C80F6291CE695EFD8FF5410AEBB535F5AF899045048E918458555455E7B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"direction" : "forward",
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
Execution Engine: sbe
```json
{
	"queryShapeHash" : "5237BA7189256856513BEEE637DD1C0C30B4C0FC91173DFD1F3CBF4F7D3713BD",
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
### Distinct results
`[ 1, 2, 3, 4, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "C1166C80F6291CE695EFD8FF5410AEBB535F5AF899045048E918458555455E7B",
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
Execution Engine: classic
```json
{
	"queryShapeHash" : "5237BA7189256856513BEEE637DD1C0C30B4C0FC91173DFD1F3CBF4F7D3713BD",
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
### Distinct results
`[ 1, 2, 3, 4, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "C1166C80F6291CE695EFD8FF5410AEBB535F5AF899045048E918458555455E7B",
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
Execution Engine: classic
```json
{
	"queryShapeHash" : "A20E7E2824DFFC29D69A849988124124B6BD117B47BB799273CE0721F71CD595",
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
Execution Engine: classic
```json
{
	"queryShapeHash" : "57403851FA5A939FC7B39B4126B525A01CC5DE9E2D5E0AD600AA913F73ED2E85",
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
Execution Engine: classic
```json
{
	"queryShapeHash" : "695E6C3392881FA7FED788D2707B4D9BCB66F614DD354FB4B0CB0CCEABDCE51C",
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
### Distinct results
`[ 1, 2, 3 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "210E9B236FB44A0D1F27C4466926AB0710FE1AEEFD88F1F03AAE7F03FE2B21A0",
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
						"[-inf, 3.0]"
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
					"[-inf, 3.0]"
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
Execution Engine: classic
```json
{
	"queryShapeHash" : "40B659E28941DA88527D1E94036E735EFD7E5FBC2B5F59C0D6C3FEC16E38544D",
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
									"[-inf, 3.0]"
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
								"[-inf, 3.0]"
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
### Distinct results
`[ 2, 4 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "2356EBF588896ECE2AE3B0CF91E478B91F8D348FCA6C47BDE8AD5BFF9CA93A86",
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
Execution Engine: sbe
```json
{
	"queryShapeHash" : "2E64CC2B78F471C8E7CD31EEA7701BC3CE5AA5B81C1EEDF188BB2AEED773DB82",
	"rejectedPlans" : [ ],
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

