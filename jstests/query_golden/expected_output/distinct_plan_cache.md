## 1. Distinct command utilizes plan cache
### DISTINCT_SCAN stored as inactive plan
### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "y" : 5 }
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "x"
			},
			"projection" : {
				
			},
			"query" : {
				"x" : {
					"$gt" : 3
				},
				"y" : 5
			},
			"sort" : {
				
			}
		},
		"isActive" : false,
		"planCacheKey" : "9D80DEB5"
	}
]
```

### DISTINCT_SCAN used as active plan
### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "y" : 5 }
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "x"
			},
			"projection" : {
				
			},
			"query" : {
				"x" : {
					"$gt" : 3
				},
				"y" : 5
			},
			"sort" : {
				
			}
		},
		"isActive" : true,
		"planCacheKey" : "9D80DEB5"
	}
]
```

## 2. distinct() uses same plan cache entry with different predicate
### Distinct on "x", with filter: { "x" : { "$gt" : 12 }, "y" : { "$lt" : 200 } }
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(12.0, inf.0]"
					],
					"y" : [
						"[-inf.0, 200.0)"
					]
				},
				"indexName" : "x_1_y_1",
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "x"
			},
			"projection" : {
				
			},
			"query" : {
				"x" : {
					"$gt" : 12
				},
				"y" : {
					"$lt" : 200
				}
			},
			"sort" : {
				
			}
		},
		"isActive" : false,
		"planCacheKey" : "71AC5E3B"
	}
]
```

### Distinct on "x", with filter: { "x" : { "$gt" : 12 }, "y" : { "$lt" : 250 } }
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(12.0, inf.0]"
					],
					"y" : [
						"[-inf.0, 250.0)"
					]
				},
				"indexName" : "x_1_y_1",
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "x"
			},
			"projection" : {
				
			},
			"query" : {
				"x" : {
					"$gt" : 12
				},
				"y" : {
					"$lt" : 250
				}
			},
			"sort" : {
				
			}
		},
		"isActive" : true,
		"planCacheKey" : "71AC5E3B"
	}
]
```

## 3. Prefer cached IXSCAN over DISTINCT_SCAN for no duplicate values in the collection
### DISTINCT_SCAN stored as inactive plan
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 105 } }
```json
[
	{
		"cachedPlan" : {
			"filter" : {
				"x" : {
					"$gt" : -1
				}
			},
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "FETCH"
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "x"
			},
			"projection" : {
				
			},
			"query" : {
				"x" : {
					"$gt" : -1
				},
				"y" : {
					"$lt" : 105
				}
			},
			"sort" : {
				
			}
		},
		"isActive" : false,
		"planCacheKey" : "E3751F93"
	}
]
```

### DISTINCT_SCAN used as active plan
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 105 } }
```json
[
	{
		"cachedPlan" : {
			"filter" : {
				"x" : {
					"$gt" : -1
				}
			},
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "FETCH"
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "x"
			},
			"projection" : {
				
			},
			"query" : {
				"x" : {
					"$gt" : -1
				},
				"y" : {
					"$lt" : 105
				}
			},
			"sort" : {
				
			}
		},
		"isActive" : true,
		"planCacheKey" : "E3751F93"
	}
]
```

## 4. Aggregation DISTINCT_SCAN utilizes plan cache
### DISTINCT_SCAN stored as inactive plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1
			},
			"query" : {
				
			},
			"sort" : {
				"a" : 1,
				"b" : 1
			}
		},
		"isActive" : false,
		"planCacheKey" : "BC1278ED"
	}
]
```

### DISTINCT_SCAN used as active plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1
			},
			"query" : {
				
			},
			"sort" : {
				"a" : 1,
				"b" : 1
			}
		},
		"isActive" : true,
		"planCacheKey" : "BC1278ED"
	}
]
```

### DISTINCT_SCAN stored as inactive plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1,
				"c" : 1
			},
			"query" : {
				
			},
			"sort" : {
				
			}
		},
		"isActive" : false,
		"planCacheKey" : "4BAD7F67"
	}
]
```

### DISTINCT_SCAN used as active plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1,
				"c" : 1
			},
			"query" : {
				
			},
			"sort" : {
				
			}
		},
		"isActive" : true,
		"planCacheKey" : "4BAD7F67"
	}
]
```

## 5. DISTINCT_SCAN with embedded FETCH utilizes plan cache
### DISTINCT_SCAN stored as inactive plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1,
				"c" : 1
			},
			"query" : {
				
			},
			"sort" : {
				
			}
		},
		"isActive" : false,
		"planCacheKey" : "35EE2431"
	}
]
```

### DISTINCT_SCAN used as active plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1,
				"c" : 1
			},
			"query" : {
				
			},
			"sort" : {
				
			}
		},
		"isActive" : true,
		"planCacheKey" : "35EE2431"
	}
]
```

### DISTINCT_SCAN stored as inactive plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1,
				"c" : 1
			},
			"query" : {
				
			},
			"sort" : {
				
			}
		},
		"isActive" : false,
		"planCacheKey" : "4BAD7F67"
	}
]
```

### DISTINCT_SCAN used as active plan
### Pipeline:
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
```json
[
	{
		"cachedPlan" : {
			"inputStage" : {
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
				"indexVersion" : 2,
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
			},
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				
			}
		},
		"createdFromQuery" : {
			"distinct" : {
				"key" : "a"
			},
			"projection" : {
				"_id" : 0,
				"a" : 1,
				"b" : 1,
				"c" : 1
			},
			"query" : {
				
			},
			"sort" : {
				
			}
		},
		"isActive" : true,
		"planCacheKey" : "4BAD7F67"
	}
]
```

