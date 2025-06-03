## 1. No DISTINCT_SCAN candidate considered
### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "z" : 5 }
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "DB842DD74C3C0E5452DDFEFE378E4176931DE3305E57FD7A484744F79A33A80C",
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
						"[inf, 3.0)"
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
						"(3.0, inf]"
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
### Distinct results
`[ 1, 2, 3, 4, 5 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "B30F9C427FBD57B85A0E86D6BAA04F5B82BC4F8A53389259D6F4B7D4D47FB479",
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
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "DB842DD74C3C0E5452DDFEFE378E4176931DE3305E57FD7A484744F79A33A80C",
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
						"[inf, 3.0)"
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
						"(3.0, inf]"
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
### Distinct results
`[ 1, 2, 3, 4, 5, 6, 7, 8 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "FA3B4605574715FF2B1F31E2F74B29E3CBFD1BAD04A99C1024D81BE22FDBCEDF",
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
### Distinct results
`[ 3, 5, 6, 7, 8 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "FA3B4605574715FF2B1F31E2F74B29E3CBFD1BAD04A99C1024D81BE22FDBCEDF",
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
### Distinct results
`[ 3 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "B30F9C427FBD57B85A0E86D6BAA04F5B82BC4F8A53389259D6F4B7D4D47FB479",
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
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "BDD4EBB6F530210B9E1E7FFC66CEF96E312531E73BAD8F5CF2AB8D388756E9E3",
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
						"[inf, 3.0)"
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
						"(3.0, inf]"
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
						"(3.0, inf]"
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
					"(3.0, inf]"
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

### Prefer DISTINCT_SCAN with combined bounds under $or
### Distinct on "x", with filter: {
	"$or" : [
		{
			"x" : {
				"$lt" : 4
			}
		},
		{
			"x" : {
				"$gt" : 6
			}
		}
	]
}
### Distinct results
`[ 3, 7, 8 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "E97E5CB50AE854BA26AD1CBDBFC96D330F0DC8EEC557435EE1F9120CDDBC61EC",
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
						"[inf, 6.0)",
						"(4.0, -inf]"
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
						"[-inf, 4.0)",
						"(6.0, inf]"
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
		],
		[
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"stage" : "OR"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(4.0, -inf]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_-1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(6.0, inf]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"stage" : "OR"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[-inf, 4.0)"
					],
					"y" : [
						"[MinKey, MaxKey]"
					],
					"z" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_z_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(6.0, inf]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"stage" : "OR"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[inf, 6.0)"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_-1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[-inf, 4.0)"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"stage" : "OR"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[-inf, 4.0)"
					],
					"y" : [
						"[MinKey, MaxKey]"
					],
					"z" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_z_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[inf, 6.0)"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_-1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"stage" : "OR"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(6.0, inf]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					],
					"z" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_z_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"[-inf, 4.0)"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			}
		],
		[
			{
				"stage" : "PROJECTION_DEFAULT",
				"transformBy" : {
					"_id" : 0,
					"x" : 1
				}
			},
			{
				"stage" : "OR"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(6.0, inf]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					],
					"z" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_1_z_1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
				"stage" : "IXSCAN"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(4.0, -inf]"
					],
					"y" : [
						"[MinKey, MaxKey]"
					]
				},
				"indexName" : "x_-1_y_1",
				"isMultiKey" : false,
				"isPartial" : false,
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
					"[-inf, 4.0)",
					"(6.0, inf]"
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

### Prefer DISTINCT_SCAN with $or -> $in optimization
### Distinct on "x", with filter: {
	"$or" : [
		{
			"x" : {
				"$eq" : 2
			}
		},
		{
			"x" : {
				"$eq" : 4
			}
		},
		{
			"x" : {
				"$eq" : 6
			}
		}
	]
}
### Distinct results
`[ 6 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "EA1833ADA541F05B87DFD1FE7853A0F0198B7E8B0BC033E4AB8D5637FE766E32",
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
						"[6.0, 6.0]",
						"[4.0, 4.0]",
						"[2.0, 2.0]"
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
						"[2.0, 2.0]",
						"[4.0, 4.0]",
						"[6.0, 6.0]"
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
					"[2.0, 2.0]",
					"[4.0, 4.0]",
					"[6.0, 6.0]"
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

### No DISTINCT_SCAN candidate considered due to rooted $or
### Distinct on "x", with filter: {
	"$or" : [
		{
			"x" : {
				"$gt" : 3
			}
		},
		{
			"y" : {
				"$eq" : 5
			}
		}
	]
}
### Distinct results
`[ 3, 5, 6, 7, 8 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "9940887E433A7A6E1B528E40AD9BFF7C3ECB6766FFB0668B9E3DA520D40F21A8",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "SUBPLAN"
		},
		{
			"stage" : "FETCH"
		},
		{
			"stage" : "OR"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[MinKey, MaxKey]"
				],
				"y" : [
					"[5.0, 5.0]"
				]
			},
			"indexName" : "y_1_x_1",
			"isMultiKey" : false,
			"isPartial" : false,
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
			"stage" : "IXSCAN"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"(3.0, inf]"
				],
				"y" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "x_1_y_1",
			"isMultiKey" : false,
			"isPartial" : false,
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
			"stage" : "IXSCAN"
		}
	]
}
```

### Distinct on "x", with filter: {
	"$or" : [
		{
			"x" : {
				"$eq" : 5
			},
			"z" : {
				"$ne" : 4
			}
		},
		{
			"y" : {
				"$lt" : 7
			}
		}
	]
}
### Distinct results
`[ 3, 5, 6, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "3BE8FF017DB0BC6A12A9DDF77664266F51915FB48B8328E61FD22C477BFE763E",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "SUBPLAN"
		},
		{
			"stage" : "FETCH"
		},
		{
			"stage" : "OR"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[MinKey, MaxKey]"
				],
				"y" : [
					"[-inf, 7.0)"
				]
			},
			"indexName" : "y_1_x_1",
			"isMultiKey" : false,
			"isPartial" : false,
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
			"stage" : "IXSCAN"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[5.0, 5.0]"
				],
				"y" : [
					"[MinKey, MaxKey]"
				],
				"z" : [
					"[MinKey, 4.0)",
					"(4.0, MaxKey]"
				]
			},
			"indexName" : "x_1_z_1_y_1",
			"isMultiKey" : false,
			"isPartial" : false,
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
			"stage" : "IXSCAN"
		}
	]
}
```

## 3. Prefer DISTINCT_SCAN for many duplicate values in the collection
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 250 } }
### Distinct results
`[ 0, 1 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "261552A4264D745A84443394C21486B6A31CB041E7EE160C2461119A15C43CC9",
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
						"(-1.0, inf]"
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
						"[-inf, 250.0)"
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
					"(-1.0, inf]"
				],
				"y" : [
					"[-inf, 250.0)"
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
### Distinct results
`[ 0, 1, 2, 3, 4 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "261552A4264D745A84443394C21486B6A31CB041E7EE160C2461119A15C43CC9",
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
						"(-1.0, inf]"
					],
					"y" : [
						"[-inf, 105.0)"
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
						"(-1.0, inf]"
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
					"[-inf, 105.0)"
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

### Maintain prior behavior even under a rooted $or
### Distinct on "x", with filter: {
	"$or" : [
		{
			"x" : {
				"$gt" : -1
			},
			"y" : {
				"$lt" : 105
			}
		},
		{
			"x" : {
				"$eq" : 0
			}
		}
	]
}
### Distinct results
`[ 0, 1, 2, 3, 4 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "0B13AF8F3D7C270576519140DC51094A14E7E8F2ABA9CA2D71F43B57117B3BDB",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "SUBPLAN"
		},
		{
			"stage" : "FETCH"
		},
		{
			"stage" : "OR"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"x" : [
					"[0.0, 0.0]"
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
		},
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
					"[-inf, 105.0)"
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
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "BDD4EBB6F530210B9E1E7FFC66CEF96E312531E73BAD8F5CF2AB8D388756E9E3",
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
					"(3.0, inf]"
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
### Distinct results
`[ 0, 1 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "261552A4264D745A84443394C21486B6A31CB041E7EE160C2461119A15C43CC9",
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
					"(-1.0, inf]"
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
### Distinct results
`[ 0, 1 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "261552A4264D745A84443394C21486B6A31CB041E7EE160C2461119A15C43CC9",
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
### Distinct results
`[ 0, 1, 2, 3, 4 ]`
### Summarized explain
```json
{
	"queryShapeHash" : "261552A4264D745A84443394C21486B6A31CB041E7EE160C2461119A15C43CC9",
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
					"(-1.0, inf]"
				],
				"y" : [
					"[-inf, 105.0)"
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

