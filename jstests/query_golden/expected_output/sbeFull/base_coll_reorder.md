## 1. 3-Node graph, base node fully connected
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$project" : {
			"_id" : 0,
			"x._id" : 0,
			"y._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "07EDFA9AFB9088CD25B2C2DC4C13581B00039C20DE4142FB326FE0BB7B7BE349",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStage" : {
			"asField" : "y",
			"foreignCollection" : "test.base_coll_reorder_md_b",
			"foreignField" : "b",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.base_coll_reorder_md_a",
				"foreignField" : "a",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.base_coll_reorder_md_base",
					"stage" : "COLLSCAN"
				},
				"localField" : "a",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "b",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"planNodeId" : 4,
		"stage" : "PROJECTION_DEFAULT",
		"transformBy" : {
			"_id" : false,
			"x" : {
				"_id" : false
			},
			"y" : {
				"_id" : false
			}
		}
	}
}
```

### Random reordering with seed 0
`(HJ _ = (HJ y = COLLSCAN, _ = COLLSCAN), x = COLLSCAN)`
### Random reordering with seed 1
`(HJ _ = (HJ x = COLLSCAN, _ = COLLSCAN), y = COLLSCAN)`
### Random reordering with seed 2
`(HJ _ = (HJ _ = COLLSCAN, x = COLLSCAN), y = COLLSCAN)`
### Random reordering with seed 7
`(HJ _ = (HJ _ = COLLSCAN, y = COLLSCAN), x = COLLSCAN)`

## 2. 3-Node graph, base node connected to one node
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "y",
			"localField" : "x.b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$project" : {
			"_id" : 0,
			"x._id" : 0,
			"y._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "b" : -11,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : -11,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "3E65060EE706660AF2949B94879998C4ABE285431BAD8C0BC3E030F868A3CD8B",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStage" : {
			"asField" : "y",
			"foreignCollection" : "test.base_coll_reorder_md_b",
			"foreignField" : "b",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.base_coll_reorder_md_a",
				"foreignField" : "a",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.base_coll_reorder_md_base",
					"stage" : "COLLSCAN"
				},
				"localField" : "a",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "x.b",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"planNodeId" : 4,
		"stage" : "PROJECTION_DEFAULT",
		"transformBy" : {
			"_id" : false,
			"x" : {
				"_id" : false
			},
			"y" : {
				"_id" : false
			}
		}
	}
}
```

### Random reordering with seed 0
`(HJ _ = (HJ y = COLLSCAN, x = COLLSCAN), _ = COLLSCAN)`
### Random reordering with seed 1
`(HJ _ = (HJ x = COLLSCAN, _ = COLLSCAN), y = COLLSCAN)`
### Random reordering with seed 2
`(HJ _ = (HJ _ = COLLSCAN, x = COLLSCAN), y = COLLSCAN)`
### Random reordering with seed 3
`(HJ _ = (HJ x = COLLSCAN, y = COLLSCAN), _ = COLLSCAN)`

## 3. 3-Node graph + potentially inferred edge
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "x",
			"localField" : "base",
			"foreignField" : "base"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "y",
			"localField" : "base",
			"foreignField" : "base"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$project" : {
			"_id" : 0,
			"x._id" : 0,
			"y._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "b" : -11,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : -1,  "b" : -1,  "base" : 33 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "38C2ABCABF327D1D7292A8A20CEC74497DDA9AE37D1067EDF51ED3B16B663A41",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStage" : {
			"asField" : "y",
			"foreignCollection" : "test.base_coll_reorder_md_b",
			"foreignField" : "base",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.base_coll_reorder_md_a",
				"foreignField" : "base",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.base_coll_reorder_md_base",
					"stage" : "COLLSCAN"
				},
				"localField" : "base",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "base",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"planNodeId" : 4,
		"stage" : "PROJECTION_DEFAULT",
		"transformBy" : {
			"_id" : false,
			"x" : {
				"_id" : false
			},
			"y" : {
				"_id" : false
			}
		}
	}
}
```

### Random reordering with seed 0
`(HJ _ = (HJ y = COLLSCAN, _ = COLLSCAN), x = COLLSCAN)`
### Random reordering with seed 1
`(HJ _ = (HJ x = COLLSCAN, _ = COLLSCAN), y = COLLSCAN)`
### Random reordering with seed 2
`(HJ _ = (HJ _ = COLLSCAN, x = COLLSCAN), y = COLLSCAN)`
### Random reordering with seed 7
`(HJ _ = (HJ _ = COLLSCAN, y = COLLSCAN), x = COLLSCAN)`

## 4. 4-Node graph + potentially inferred edges & filters
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"b" : {
				"$eq" : 3
			}
		}
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "x",
			"localField" : "base",
			"foreignField" : "base"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "y",
			"localField" : "base",
			"foreignField" : "base"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_base",
			"as" : "z",
			"localField" : "y.base",
			"foreignField" : "base",
			"pipeline" : [
				{
					"$match" : {
						"base" : {
							"$gt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$z"
	},
	{
		"$project" : {
			"_id" : 0,
			"x._id" : 0,
			"y._id" : 0,
			"z._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "z" : {  "a" : 2,  "b" : -11,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "z" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : -1,  "b" : -1,  "base" : 33 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 },  "z" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "CE80C256CEE612EF2CFC2F36A71A484F5A11422A9CC5D829527592AFED3B2286",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"asField" : "y",
					"foreignCollection" : "test.base_coll_reorder_md_b",
					"foreignField" : "base",
					"inputStage" : {
						"asField" : "x",
						"foreignCollection" : "test.base_coll_reorder_md_a",
						"foreignField" : "base",
						"inputStage" : {
							"direction" : "forward",
							"filter" : {
								"b" : {
									"$eq" : 3
								}
							},
							"nss" : "test.base_coll_reorder_md_base",
							"stage" : "COLLSCAN"
						},
						"localField" : "base",
						"scanDirection" : "forward",
						"stage" : "EQ_LOOKUP_UNWIND",
						"strategy" : "HashJoin"
					},
					"localField" : "base",
					"planNodeId" : 3,
					"scanDirection" : "forward",
					"stage" : "EQ_LOOKUP_UNWIND",
					"strategy" : "HashJoin"
				}
			}
		},
		{
			"$lookup" : {
				"as" : "z",
				"foreignField" : "base",
				"from" : "base_coll_reorder_md_base",
				"let" : {
					
				},
				"localField" : "y.base",
				"pipeline" : [
					{
						"$match" : {
							"base" : {
								"$gt" : 3
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$project" : {
				"_id" : false,
				"x" : {
					"_id" : false
				},
				"y" : {
					"_id" : false
				},
				"z" : {
					"_id" : false
				}
			}
		}
	]
}
```

### Random reordering with seed 0
`(HJ _ = (HJ _ = (HJ _ = COLLSCAN, x = COLLSCAN), y = COLLSCAN), z = COLLSCAN)`
### Random reordering with seed 1
`(HJ _ = (HJ _ = (HJ x = COLLSCAN, _ = COLLSCAN), y = COLLSCAN), z = COLLSCAN)`
### Random reordering with seed 3
`(HJ _ = (HJ _ = (HJ y = COLLSCAN, z = COLLSCAN), _ = COLLSCAN), x = COLLSCAN)`
### Random reordering with seed 5
`(HJ _ = (HJ _ = (HJ z = COLLSCAN, y = COLLSCAN), _ = COLLSCAN), x = COLLSCAN)`
### Random reordering with seed 6
`(HJ _ = (HJ _ = (HJ y = COLLSCAN, _ = COLLSCAN), x = COLLSCAN), z = COLLSCAN)`

## 5. 5-Node graph + filters
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"b" : {
				"$eq" : 3
			}
		}
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "aaa",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"base" : {
							"$in" : [
								22,
								33
							]
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$aaa"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "bbb",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"base" : {
							"$gt" : 20
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$bbb"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_base",
			"as" : "ccc",
			"localField" : "aaa.base",
			"foreignField" : "base",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$lt" : 0
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$ccc"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "ddd",
			"localField" : "base",
			"foreignField" : "base",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : 0
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$ddd"
	},
	{
		"$project" : {
			"_id" : 0,
			"aaa._id" : 0,
			"bbb._id" : 0,
			"ccc._id" : 0,
			"ddd._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "aaa" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "b" : 3,  "base" : 22,  "bbb" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "ccc" : {  "a" : 2,  "b" : -11,  "base" : 22 },  "ddd" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "aaa" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "b" : 3,  "base" : 22,  "bbb" : {  "a" : 2,  "b" : 3,  "base" : 33 },  "ccc" : {  "a" : 2,  "b" : -11,  "base" : 22 },  "ddd" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "aaa" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "b" : 3,  "base" : 33,  "bbb" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "ccc" : {  "a" : 2,  "b" : -11,  "base" : 22 },  "ddd" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "aaa" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "b" : 3,  "base" : 33,  "bbb" : {  "a" : 2,  "b" : 3,  "base" : 33 },  "ccc" : {  "a" : 2,  "b" : -11,  "base" : 22 },  "ddd" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "7F8120088DB6BDABF7B9AE316C68CA25A76CAB0228D05ABA012F7D28BEC03A14",
	"stages" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : {
					"direction" : "forward",
					"filter" : {
						"b" : {
							"$eq" : 3
						}
					},
					"nss" : "test.base_coll_reorder_md_base",
					"planNodeId" : 1,
					"stage" : "COLLSCAN"
				}
			}
		},
		{
			"$lookup" : {
				"as" : "aaa",
				"foreignField" : "a",
				"from" : "base_coll_reorder_md_a",
				"let" : {
					
				},
				"localField" : "a",
				"pipeline" : [
					{
						"$match" : {
							"base" : {
								"$in" : [
									22,
									33
								]
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$lookup" : {
				"as" : "bbb",
				"foreignField" : "b",
				"from" : "base_coll_reorder_md_b",
				"let" : {
					
				},
				"localField" : "b",
				"pipeline" : [
					{
						"$match" : {
							"base" : {
								"$gt" : 20
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$lookup" : {
				"as" : "ccc",
				"foreignField" : "base",
				"from" : "base_coll_reorder_md_base",
				"let" : {
					
				},
				"localField" : "aaa.base",
				"pipeline" : [
					{
						"$match" : {
							"b" : {
								"$lt" : 0
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$lookup" : {
				"as" : "ddd",
				"foreignField" : "base",
				"from" : "base_coll_reorder_md_b",
				"let" : {
					
				},
				"localField" : "base",
				"pipeline" : [
					{
						"$match" : {
							"b" : {
								"$gt" : 0
							}
						}
					}
				],
				"unwinding" : {
					"preserveNullAndEmptyArrays" : false
				}
			}
		},
		{
			"$project" : {
				"_id" : false,
				"aaa" : {
					"_id" : false
				},
				"bbb" : {
					"_id" : false
				},
				"ccc" : {
					"_id" : false
				},
				"ddd" : {
					"_id" : false
				}
			}
		}
	]
}
```

### Random reordering with seed 0
`(HJ _ = (HJ _ = (HJ _ = (HJ ddd = COLLSCAN, _ = COLLSCAN), aaa = COLLSCAN), ccc = COLLSCAN), bbb = COLLSCAN)`
### Random reordering with seed 1
`(HJ _ = (HJ _ = (HJ _ = (HJ _ = COLLSCAN, aaa = COLLSCAN), ccc = COLLSCAN), bbb = COLLSCAN), ddd = COLLSCAN)`
### Random reordering with seed 2
`(HJ _ = (HJ _ = (HJ _ = (HJ ccc = COLLSCAN, aaa = COLLSCAN), _ = COLLSCAN), bbb = COLLSCAN), ddd = COLLSCAN)`
### Random reordering with seed 3
`(HJ _ = (HJ _ = (HJ _ = (HJ aaa = COLLSCAN, ccc = COLLSCAN), _ = COLLSCAN), bbb = COLLSCAN), ddd = COLLSCAN)`
### Random reordering with seed 4
`(HJ _ = (HJ _ = (HJ _ = (HJ _ = COLLSCAN, bbb = COLLSCAN), aaa = COLLSCAN), ccc = COLLSCAN), ddd = COLLSCAN)`
### Random reordering with seed 7
`(HJ _ = (HJ _ = (HJ _ = (HJ _ = COLLSCAN, ddd = COLLSCAN), aaa = COLLSCAN), ccc = COLLSCAN), bbb = COLLSCAN)`
### Random reordering with seed 8
`(HJ _ = (HJ _ = (HJ _ = (HJ ccc = COLLSCAN, aaa = COLLSCAN), _ = COLLSCAN), ddd = COLLSCAN), bbb = COLLSCAN)`
### Random reordering with seed 9
`(HJ _ = (HJ _ = (HJ _ = (HJ _ = COLLSCAN, ddd = COLLSCAN), bbb = COLLSCAN), aaa = COLLSCAN), ccc = COLLSCAN)`
### Random reordering with seed 10
`(HJ _ = (HJ _ = (HJ _ = (HJ bbb = COLLSCAN, _ = COLLSCAN), aaa = COLLSCAN), ccc = COLLSCAN), ddd = COLLSCAN)`
### Random reordering with seed 11
`(HJ _ = (HJ _ = (HJ _ = (HJ aaa = COLLSCAN, _ = COLLSCAN), ddd = COLLSCAN), bbb = COLLSCAN), ccc = COLLSCAN)`

