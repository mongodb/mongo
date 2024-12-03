## 1. No DISTINCT_SCAN candidate considered
### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "z" : 5 }
### Expected results
`[ 5, 6, 7 ]`
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"filter" : {
					"z" : {
						"$eq" : 5
					}
				},
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[inf.0, 3.0)"
					]
				},
				"indexName" : "x_-1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : -1
				},
				"multiKeyPaths" : {
					"x" : [ ]
				},
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"filter" : {
					"z" : {
						"$eq" : 5
					}
				},
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(3.0, inf.0]"
					]
				},
				"indexName" : "x_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ]
				},
				"stage" : "IXSCAN"
			}
		]
	],
	"winningPlan" : [
		{
			"filter" : {
				"x" : {
					"$gt" : 3
				}
			},
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"y" : [
					"[MinKey, MaxKey]"
				],
				"z" : [
					"[5.0, 5.0]"
				]
			},
			"indexName" : "z_1_y_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"y" : 1,
				"z" : 1
			},
			"multiKeyPaths" : {
				"y" : [ ],
				"z" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

### No DISTINCT_SCAN candidate considered due to multikeyness
### Distinct on "x", with filter: { "x" : 3 }
### Expected results
`[ 1, 2, 3, 4, 5 ]`
### Distinct results
`[ 1, 2, 3, 4, 5 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[3.0, 3.0]"
					]
				},
				"indexName" : "x_-1",
				"isMultiKey" : true,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : -1
				},
				"multiKeyPaths" : {
					"x" : [
						"x"
					]
				},
				"stage" : "IXSCAN"
			}
		]
	],
	"winningPlan" : [
		{
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[3.0, 3.0]"
				]
			},
			"indexName" : "x_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1
			},
			"multiKeyPaths" : {
				"x" : [
					"x"
				]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "z" : 5 }
### Expected results
`[ 5, 6, 7 ]`
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"filter" : {
					"z" : {
						"$eq" : 5
					}
				},
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[inf.0, 3.0)"
					]
				},
				"indexName" : "x_-1",
				"isMultiKey" : true,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : -1
				},
				"multiKeyPaths" : {
					"x" : [
						"x"
					]
				},
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"filter" : {
					"z" : {
						"$eq" : 5
					}
				},
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(3.0, inf.0]"
					]
				},
				"indexName" : "x_1",
				"isMultiKey" : true,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1
				},
				"multiKeyPaths" : {
					"x" : [
						"x"
					]
				},
				"stage" : "IXSCAN"
			}
		]
	],
	"winningPlan" : [
		{
			"filter" : {
				"x" : {
					"$gt" : 3
				}
			},
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"y" : [
					"[MinKey, MaxKey]"
				],
				"z" : [
					"[5.0, 5.0]"
				]
			},
			"indexName" : "z_1_y_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"y" : 1,
				"z" : 1
			},
			"multiKeyPaths" : {
				"y" : [ ],
				"z" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

### Only DISTINCT_SCAN candidates considered despite multikeyness
### Distinct on "x", with filter: { }
### Expected results
`[ 1, 2, 3, 4, 5, 6, 7, 8 ]`
### Distinct results
`[ 1, 2, 3, 4, 5, 6, 7, 8 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : 0,
				"x" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "x_1",
			"isFetching" : false,
			"isMultiKey" : true,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1
			},
			"multiKeyPaths" : {
				"x" : [
					"x"
				]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

## 2. Only DISTINCT_SCAN candidates considered
### Distinct on "x", with filter: { }
### Expected results
`[ 3, 5, 6, 7, 8 ]`
### Distinct results
`[ 3, 5, 6, 7, 8 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : 0,
				"x" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[MinKey, MaxKey]"
				],
				"y" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "x_1_y_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1,
				"y" : 1
			},
			"multiKeyPaths" : {
				"x" : [ ],
				"y" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

### Distinct on "x", with filter: { "x" : 3 }
### Expected results
`[ 3 ]`
### Distinct results
`[ 3 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[3.0, 3.0]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_-1_y_1",
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : -1,
					"y" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ],
					"y" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[3.0, 3.0]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					],
					"z" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_z_1_y_1",
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1,
					"y" : 1,
					"z" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ],
					"y" : [ ],
					"z" : [ ]
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
				"x" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[3.0, 3.0]"
				],
				"y" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "x_1_y_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1,
				"y" : 1
			},
			"multiKeyPaths" : {
				"x" : [ ],
				"y" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "y" : 5 }
### Expected results
`[ 5, 6, 7 ]`
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[inf.0, 3.0)"
					],
					"y" : [
						"[5.0, 5.0]"
					]
				},
				"indexName" : "x_-1_y_1",
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : -1,
					"y" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ],
					"y" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(3.0, inf.0]"
					],
					"y" : [
						"[5.0, 5.0]"
					]
				},
				"indexName" : "x_1_y_1",
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1,
					"y" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ],
					"y" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(3.0, inf.0]"
					],
					"y" : [
						"[5.0, 5.0]"
					],
					"z" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_z_1_y_1",
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1,
					"y" : 1,
					"z" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ],
					"y" : [ ],
					"z" : [ ]
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
				"x" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"(3.0, inf.0]"
				],
				"y" : [
					"[5.0, 5.0]"
				]
			},
			"indexName" : "y_1_x_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1,
				"y" : 1
			},
			"multiKeyPaths" : {
				"x" : [ ],
				"y" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

## 3. Prefer DISTINCT_SCAN for many duplicate values in the collection
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 250 } }
### Expected results
`[ 0, 1 ]`
### Distinct results
`[ 0, 1 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"filter" : {
					"y" : {
						"$lt" : 250
					}
				},
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(-1.0, inf.0]"
					]
				},
				"indexName" : "x_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ]
				},
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"filter" : {
					"x" : {
						"$gt" : -1
					}
				},
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"y" : [
						"[-inf.0, 250.0)"
					],
					"z" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "y_1_z_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"y" : 1,
					"z" : 1
				},
				"multiKeyPaths" : {
					"y" : [ ],
					"z" : [ ]
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
				"x" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"(-1.0, inf.0]"
				],
				"y" : [
					"[-inf.0, 250.0)"
				]
			},
			"indexName" : "x_1_y_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1,
				"y" : 1
			},
			"multiKeyPaths" : {
				"x" : [ ],
				"y" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

## 4. Prefer FETCH + filter + IXSCAN for more selective predicate on y
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 105 } }
### Expected results
`[ 0, 1, 2, 3, 4 ]`
### Distinct results
`[ 0, 1, 2, 3, 4 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "PROJECTION_COVERED",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(-1.0, inf.0]"
					],
					"y" : [
						"[-inf.0, 105.0)"
					]
				},
				"indexName" : "x_1_y_1",
				"isFetching" : false,
				"isMultiKey" : false,
				"isPartial" : false,
				"isShardFiltering" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1,
					"y" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ],
					"y" : [ ]
				},
				"stage" : "DISTINCT_SCAN"
			}
		],
		[
			{
				"filter" : {
					"y" : {
						"$lt" : 105
					}
				},
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(-1.0, inf.0]"
					]
				},
				"indexName" : "x_1",
				"isMultiKey" : false,
				"isPartial" : false,
				"isSparse" : false,
				"isUnique" : false,
				"keyPattern" : {
					"x" : 1
				},
				"multiKeyPaths" : {
					"x" : [ ]
				},
				"stage" : "IXSCAN"
			}
		]
	],
	"winningPlan" : [
		{
			"filter" : {
				"x" : {
					"$gt" : -1
				}
			},
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"y" : [
					"[-inf.0, 105.0)"
				],
				"z" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "y_1_z_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"y" : 1,
				"z" : 1
			},
			"multiKeyPaths" : {
				"y" : [ ],
				"z" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

## 5. Use hinted DISTINCT_SCAN
### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "y" : 5 }, and options: { "hint" : { "x" : 1, "y" : 1 } }
### Expected results
`[ 5, 6, 7 ]`
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : 0,
				"x" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"(3.0, inf.0]"
				],
				"y" : [
					"[5.0, 5.0]"
				]
			},
			"indexName" : "x_1_y_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1,
				"y" : 1
			},
			"multiKeyPaths" : {
				"x" : [ ],
				"y" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

## 6. Use hinted IXSCAN, even with preferable DISTINCT_SCAN
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 250 } }, and options: { "hint" : { "x" : 1 } }
### Expected results
`[ 0, 1 ]`
### Distinct results
`[ 0, 1 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"filter" : {
				"y" : {
					"$lt" : 250
				}
			},
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"(-1.0, inf.0]"
				]
			},
			"indexName" : "x_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1
			},
			"multiKeyPaths" : {
				"x" : [ ]
			},
			"stage" : "IXSCAN"
		}
	]
}
```

## 7. Use hinted COLLSCAN, even with preferable DISTINCT_SCAN
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 250 } }, and options: { "hint" : { "$natural" : 1 } }
### Expected results
`[ 0, 1 ]`
### Distinct results
`[ 0, 1 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"direction" : "forward",
			"filter" : {
				"$and" : [
					{
						"y" : {
							"$lt" : 250
						}
					},
					{
						"x" : {
							"$gt" : -1
						}
					}
				]
			},
			"stage" : "COLLSCAN"
		}
	]
}
```

## 8. Use hinted DISTINCT_SCAN, even with no duplicate values
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 105 } }, and options: { "hint" : { "x" : 1, "y" : 1 } }
### Expected results
`[ 0, 1, 2, 3, 4 ]`
### Distinct results
`[ 0, 1, 2, 3, 4 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : 0,
				"x" : 1
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"(-1.0, inf.0]"
				],
				"y" : [
					"[-inf.0, 105.0)"
				]
			},
			"indexName" : "x_1_y_1",
			"isFetching" : false,
			"isMultiKey" : false,
			"isPartial" : false,
			"isShardFiltering" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"x" : 1,
				"y" : 1
			},
			"multiKeyPaths" : {
				"x" : [ ],
				"y" : [ ]
			},
			"stage" : "DISTINCT_SCAN"
		}
	]
}
```

