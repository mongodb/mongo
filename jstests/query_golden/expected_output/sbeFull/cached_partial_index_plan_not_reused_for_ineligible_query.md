## 1. Collection setup
Inserting documents
```json
[ { "_id" : 1, "a" : 0 } ]
```
## 2. Queries with no indexes
### Query
```json
{
	"$or" : [
		{
			"a" : 1
		},
		{
			"a" : {
				"$lte" : "a string"
			}
		}
	],
	"_id" : {
		"$lte" : 5
	}
}
```
### Results
```json
[ ]
```
### Query
```json
{
	"$or" : [
		{
			"a" : 1
		},
		{
			"a" : {
				"$lte" : 10
			}
		}
	],
	"_id" : {
		"$lte" : 5
	}
}
```
### Results
```json
[ { "_id" : 1, "a" : 0 } ]
```
## 3. Index setup
Creating Index
```json
{
	"a" : 1,
	"partialFilterExpression" : {
		"$or" : [
			{
				"a" : 1
			},
			{
				"a" : {
					"$lte" : "a string"
				}
			}
		]
	}
}
```
## 4. Queries with partial index
### Query
```json
{
	"$or" : [
		{
			"a" : 1
		},
		{
			"a" : {
				"$lte" : "a string"
			}
		}
	],
	"_id" : {
		"$lte" : 5
	}
}
```
### Results
```json
[ ]
```
### Query
```json
{
	"$or" : [
		{
			"a" : 1
		},
		{
			"a" : {
				"$lte" : "a string"
			}
		}
	],
	"_id" : {
		"$lte" : 5
	}
}
```
### Results
```json
[ ]
```
### Query
```json
{
	"$or" : [
		{
			"a" : 1
		},
		{
			"a" : {
				"$lte" : "a string"
			}
		}
	],
	"_id" : {
		"$lte" : 5
	}
}
```
### Results
```json
[ ]
```
### Explain
```json
{
	"stage" : "FETCH",
	"planNodeId" : 2,
	"filter" : {
		"_id" : {
			"$lte" : 5
		}
	},
	"inputStage" : {
		"stage" : "IXSCAN",
		"planNodeId" : 1,
		"keyPattern" : {
			"a" : 1
		},
		"indexName" : "a_1",
		"isMultiKey" : false,
		"multiKeyPaths" : {
			"a" : [ ]
		},
		"isUnique" : false,
		"isSparse" : false,
		"isPartial" : true,
		"indexVersion" : 2,
		"direction" : "forward",
		"indexBounds" : {
			"a" : [
				"[1.0, 1.0]",
				"[\"\", \"a string\"]"
			]
		}
	}
}
```
### Plan cache
Verifying that the plan cache contains an entry with the partial index
```json
[
	{
		"cachedPlan" : {
			"filter" : {
				"_id" : {
					"$lte" : 5
				}
			},
			"inputStage" : {
				"direction" : "forward",
				"indexBounds" : {
					"a" : [
						"[1.0, 1.0]",
						"[\"\", \"a string\"]"
					]
				},
				"indexName" : "a_1",
				"indexVersion" : 2,
				"isMultiKey" : false,
				"isPartial" : true,
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
			"stage" : "FETCH"
		},
		"createdFromQuery" : {
			"projection" : {
				
			},
			"query" : {
				"$or" : [
					{
						"a" : 1
					},
					{
						"a" : {
							"$lte" : "a string"
						}
					}
				],
				"_id" : {
					"$lte" : 5
				}
			},
			"sort" : {
				
			}
		},
		"isActive" : true,
		"planCacheKey" : "7D81D3A7"
	}
]
```

Verify that 2nd query does not use cached partial index plan and returns the correct document
### Query
```json
{
	"$or" : [
		{
			"a" : 1
		},
		{
			"a" : {
				"$lte" : 10
			}
		}
	],
	"_id" : {
		"$lte" : 5
	}
}
```
### Results
```json
[ { "_id" : 1, "a" : 0 } ]
```
## 5. Collection setup
Inserting documents
```json
[ { "_id" : 1, "a" : 0 } ]
```
Creating Index
```json
{
	"spec" : {
		"a" : 1
	},
	"partialFilterExpression" : {
		"$or" : [
			{
				"a" : {
					"$lt" : "a"
				}
			},
			{
				"_id" : {
					"$eq" : 0
				},
				"a" : {
					"$eq" : 0
				}
			}
		]
	}
}
```
Creating Index
```json
{
	"spec" : {
		"a" : 1,
		"m" : 1
	},
	"partialFilterExpression" : {
		"$or" : [
			{
				"a" : {
					"$lt" : "a"
				}
			},
			{
				"_id" : {
					"$eq" : 0
				},
				"a" : {
					"$eq" : 0
				}
			}
		]
	}
}
```
Creating Index
```json
{
	"spec" : {
		"b" : 1,
		"a" : 1
	},
	"partialFilterExpression" : {
		"$or" : [
			{
				"a" : {
					"$lt" : "a"
				}
			},
			{
				"_id" : {
					"$eq" : 0
				},
				"a" : {
					"$eq" : 0
				}
			}
		]
	}
}
```
### Aggregation
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$lt" : "a"
					}
				},
				{
					"_id" : {
						"$eq" : 0
					},
					"a" : {
						"$eq" : -1
					}
				}
			]
		}
	},
	{
		"$sort" : {
			"b" : 1
		}
	}
]
```
### Results
```json
[ ]
```
### Aggregation
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$lt" : "a"
					}
				},
				{
					"_id" : {
						"$eq" : 0
					},
					"a" : {
						"$eq" : -1
					}
				}
			]
		}
	},
	{
		"$sort" : {
			"b" : 1
		}
	}
]
```
### Results
```json
[ ]
```
### Aggregation
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$lt" : "a"
					}
				},
				{
					"_id" : {
						"$eq" : 0
					},
					"a" : {
						"$eq" : -1
					}
				}
			]
		}
	},
	{
		"$sort" : {
			"b" : 1
		}
	}
]
```
### Results
```json
[ ]
```
### Plan cache
Verifying that the plan cache contains an entry with the partial index
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
				"inputStage" : {
					"inputStages" : [
						{
							"filter" : {
								"a" : {
									"$eq" : -1
								}
							},
							"inputStage" : {
								"direction" : "forward",
								"indexBounds" : {
									"_id" : [
										"[0.0, 0.0]"
									]
								},
								"indexName" : "_id_",
								"indexVersion" : 2,
								"isMultiKey" : false,
								"isPartial" : false,
								"isSparse" : false,
								"isUnique" : true,
								"keyPattern" : {
									"_id" : 1
								},
								"stage" : "IXSCAN"
							},
							"stage" : "FETCH"
						},
						{
							"direction" : "forward",
							"indexBounds" : {
								"a" : [
									"[\"\", \"a\")"
								]
							},
							"indexName" : "a_1",
							"indexVersion" : 2,
							"isMultiKey" : false,
							"isPartial" : true,
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
					"stage" : "OR"
				},
				"stage" : "FETCH"
			},
			"memLimit" : 104857600,
			"sortPattern" : {
				"b" : 1
			},
			"stage" : "SORT",
			"type" : "simple"
		},
		"createdFromQuery" : {
			"projection" : {
				
			},
			"query" : {
				"$or" : [
					{
						"a" : {
							"$lt" : "a"
						}
					},
					{
						"_id" : {
							"$eq" : 0
						},
						"a" : {
							"$eq" : -1
						}
					}
				]
			},
			"sort" : {
				"b" : 1
			}
		},
		"isActive" : true,
		"planCacheKey" : "BFDF7AB4"
	}
]
```

Verify that 2nd pipeline does not use cached partial index plan and returns the correct document
### Aggregation
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"a" : {
						"$lt" : 1
					}
				},
				{
					"_id" : {
						"$eq" : 0
					},
					"a" : {
						"$eq" : 0
					}
				}
			]
		}
	},
	{
		"$sort" : {
			"b" : 1
		}
	}
]
```
### Results
```json
[ { "_id" : 1, "a" : 0 } ]
```
