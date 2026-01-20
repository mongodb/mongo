## 1. Find filter
```json
{  "$expr" : {  "$in" : [ 1, "$m" ] } }
```
### Query knob off
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : 1 }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "CAB0CA88D371E9913F6A2EDBD915529A5997210CA3F24EAB63BF1CE6A95E642D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : 1
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : 1 } }, { "$expr" : { "$in" : [ { "$const" : 1 }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "CAB0CA88D371E9913F6A2EDBD915529A5997210CA3F24EAB63BF1CE6A95E642D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : 1
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[1.0, 1.0]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 2. Find filter
```json
{  "$expr" : {  "$in" : [ null, "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ 4, 5, 6, null, 10 ] },
 {  "m" : [ null, null, null ] },
 {  "m" : [ [ null ], null ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : null }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "9AE4406AA12D6ACBA86CC4F65646C260336A5E600DFB0AD24CA4F23FF9890EEE",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : null
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ 4, 5, 6, null, 10 ] },
 {  "m" : [ null, null, null ] },
 {  "m" : [ [ null ], null ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : null } }, { "$expr" : { "$in" : [ { "$const" : null }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "9AE4406AA12D6ACBA86CC4F65646C260336A5E600DFB0AD24CA4F23FF9890EEE",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : null
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : null
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[null, null]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 3. Find filter
```json
{  "$expr" : {  "$in" : [ [ ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "E6821EF9D6C6C4CDA6CBFDB99042A8CD1CF35CD47CCD841101850F1BF5FF658F",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [ ]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ ] } }, { "$expr" : { "$in" : [ { "$const" : [ ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "E6821EF9D6C6C4CDA6CBFDB99042A8CD1CF35CD47CCD841101850F1BF5FF658F",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [ ]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [ ]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[undefined, undefined]",
					"[[], []]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 4. Find filter
```json
{  "$expr" : {  "$in" : [ [ 1 ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ 1 ] ] },
 {  "m" : [ [ 1 ], [ 2 ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ 1 ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "6061C7642A45F0C552A2B27C2D0D95DEC4103AF55835DD9E44FDBBD56734483B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								1
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ 1 ] ] },
 {  "m" : [ [ 1 ], [ 2 ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ 1 ] } }, { "$expr" : { "$in" : [ { "$const" : [ 1 ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "6061C7642A45F0C552A2B27C2D0D95DEC4103AF55835DD9E44FDBBD56734483B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								1
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										1
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[1.0, 1.0]",
					"[[ 1.0 ], [ 1.0 ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 5. Find filter
```json
{  "$expr" : {  "$in" : [ [ 2 ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ 2 ] ] },
 {  "m" : [ [ 1 ], [ 2 ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ 2 ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "6061C7642A45F0C552A2B27C2D0D95DEC4103AF55835DD9E44FDBBD56734483B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								2
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ 2 ] ] },
 {  "m" : [ [ 1 ], [ 2 ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ 2 ] } }, { "$expr" : { "$in" : [ { "$const" : [ 2 ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "6061C7642A45F0C552A2B27C2D0D95DEC4103AF55835DD9E44FDBBD56734483B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								2
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										2
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[2.0, 2.0]",
					"[[ 2.0 ], [ 2.0 ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 6. Find filter
```json
{  "$expr" : {  "$in" : [ [ null ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ null ], null ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ null ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "3A2304BBB93A5D5AB57474E0A244ED865231E9D7F361F5EF6783F264884889CA",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								null
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ null ], null ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ null ] } }, { "$expr" : { "$in" : [ { "$const" : [ null ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "3A2304BBB93A5D5AB57474E0A244ED865231E9D7F361F5EF6783F264884889CA",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								null
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										null
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[null, null]",
					"[[ null ], [ null ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 7. Find filter
```json
{  "$expr" : {  "$in" : [ [ 1, 2 ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ 1, 2 ] ] },
 {  "m" : [ [ 1, 2, 3, 4 ], [ 1, 2 ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ 1, 2 ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "6061C7642A45F0C552A2B27C2D0D95DEC4103AF55835DD9E44FDBBD56734483B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								1,
								2
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ 1, 2 ] ] },
 {  "m" : [ [ 1, 2, 3, 4 ], [ 1, 2 ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ 1, 2 ] } }, { "$expr" : { "$in" : [ { "$const" : [ 1, 2 ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "6061C7642A45F0C552A2B27C2D0D95DEC4103AF55835DD9E44FDBBD56734483B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								1,
								2
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										1,
										2
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[1.0, 1.0]",
					"[[ 1.0, 2.0 ], [ 1.0, 2.0 ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 8. Find filter
```json
{  "$expr" : {  "$in" : [ [ [ 1 ] ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ [ 1 ] ] ] },
 {  "m" : [ [ [ 1 ] ], [ [ 2 ] ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ [ 1 ] ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "90114C24B1F2EB8B3EBCD0F8387199B8C85D3D202DD68126CD9143AFC684E6BF",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								[
									1
								]
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ [ 1 ] ] ] },
 {  "m" : [ [ [ 1 ] ], [ [ 2 ] ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ [ 1 ] ] } }, { "$expr" : { "$in" : [ { "$const" : [ [ 1 ] ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "90114C24B1F2EB8B3EBCD0F8387199B8C85D3D202DD68126CD9143AFC684E6BF",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								[
									1
								]
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										[
											1
										]
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[[ 1.0 ], [ 1.0 ]]",
					"[[ [ 1.0 ] ], [ [ 1.0 ] ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 9. Find filter
```json
{  "$expr" : {  "$in" : [ [ [ 1, 2 ] ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ [ 1, 2 ] ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ [ 1, 2 ] ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "90114C24B1F2EB8B3EBCD0F8387199B8C85D3D202DD68126CD9143AFC684E6BF",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								[
									1,
									2
								]
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ [ 1, 2 ] ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ [ 1, 2 ] ] } }, { "$expr" : { "$in" : [ { "$const" : [ [ 1, 2 ] ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "90114C24B1F2EB8B3EBCD0F8387199B8C85D3D202DD68126CD9143AFC684E6BF",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								[
									1,
									2
								]
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										[
											1,
											2
										]
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[[ 1.0, 2.0 ], [ 1.0, 2.0 ]]",
					"[[ [ 1.0, 2.0 ] ], [ [ 1.0, 2.0 ] ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 10. Find filter
```json
{  "$expr" : {  "$in" : [ [ [ [ 1 ] ] ], "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ [ [ 1 ] ] ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "90114C24B1F2EB8B3EBCD0F8387199B8C85D3D202DD68126CD9143AFC684E6BF",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								[
									[
										1
									]
								]
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ [ [ 1 ] ] ] } }, { "$expr" : { "$in" : [ { "$const" : [ [ [ 1 ] ] ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "90114C24B1F2EB8B3EBCD0F8387199B8C85D3D202DD68126CD9143AFC684E6BF",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								[
									[
										1
									]
								]
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										[
											[
												1
											]
										]
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[[ [ 1.0 ] ], [ [ 1.0 ] ]]",
					"[[ [ [ 1.0 ] ] ], [ [ [ 1.0 ] ] ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 11. Find filter
```json
{  "$expr" : {  "$in" : [ {  }, "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ {  } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : {  } }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "8D1AF66905A9E235D9D2A83064E2923988FDC77D66F5F0478504FA33273026D5",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								
							}
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ {  } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : {  } } }, { "$expr" : { "$in" : [ { "$const" : {  } }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "8D1AF66905A9E235D9D2A83064E2923988FDC77D66F5F0478504FA33273026D5",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								
							}
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[{}, {}]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 12. Find filter
```json
{  "$expr" : {  "$in" : [ [ {  } ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ {  } ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ {  } ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "B238149F99D591BD15350B89D6C00FCD3D5731EBE0D7C49A97D434D2D93B04A4",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								{
									
								}
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ {  } ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ {  } ] } }, { "$expr" : { "$in" : [ { "$const" : [ {  } ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "B238149F99D591BD15350B89D6C00FCD3D5731EBE0D7C49A97D434D2D93B04A4",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								{
									
								}
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										{
											
										}
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[{}, {}]",
					"[[ {} ], [ {} ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 13. Find filter
```json
{  "$expr" : {  "$in" : [ { "a" : 1 }, "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ { "a" : 1 } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : { "a" : 1 } }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "7905AD4C28AEF75871DA6A6B1D47DA716508D63DC127283DB64AA367B67CF90C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								"a" : 1
							}
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ { "a" : 1 } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : { "a" : 1 } } }, { "$expr" : { "$in" : [ { "$const" : { "a" : 1 } }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "7905AD4C28AEF75871DA6A6B1D47DA716508D63DC127283DB64AA367B67CF90C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								"a" : 1
							}
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[{ a: 1.0 }, { a: 1.0 }]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 14. Find filter
```json
{  "$expr" : {  "$in" : [ [ { "a" : 1 } ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ { "a" : 1 } ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ { "a" : 1 } ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "B238149F99D591BD15350B89D6C00FCD3D5731EBE0D7C49A97D434D2D93B04A4",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								{
									"a" : 1
								}
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ { "a" : 1 } ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ { "a" : 1 } ] } }, { "$expr" : { "$in" : [ { "$const" : [ { "a" : 1 } ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "B238149F99D591BD15350B89D6C00FCD3D5731EBE0D7C49A97D434D2D93B04A4",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								{
									"a" : 1
								}
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										{
											"a" : 1
										}
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[{ a: 1.0 }, { a: 1.0 }]",
					"[[ { a: 1.0 } ], [ { a: 1.0 } ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 15. Find filter
```json
{  "$expr" : {  "$in" : [ { "a" : 1, "b" : 1 }, "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ { "a" : 1, "b" : 1 } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : { "a" : 1, "b" : 1 } }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "7905AD4C28AEF75871DA6A6B1D47DA716508D63DC127283DB64AA367B67CF90C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								"a" : 1,
								"b" : 1
							}
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ { "a" : 1, "b" : 1 } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : { "a" : 1, "b" : 1 } } }, { "$expr" : { "$in" : [ { "$const" : { "a" : 1, "b" : 1 } }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "7905AD4C28AEF75871DA6A6B1D47DA716508D63DC127283DB64AA367B67CF90C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								"a" : 1,
								"b" : 1
							}
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[{ a: 1.0, b: 1.0 }, { a: 1.0, b: 1.0 }]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 16. Find filter
```json
{  "$expr" : {  "$in" : [ [ {  } ], "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ [ {  } ] ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ {  } ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "B238149F99D591BD15350B89D6C00FCD3D5731EBE0D7C49A97D434D2D93B04A4",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								{
									
								}
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ [ {  } ] ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ {  } ] } }, { "$expr" : { "$in" : [ { "$const" : [ {  } ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "B238149F99D591BD15350B89D6C00FCD3D5731EBE0D7C49A97D434D2D93B04A4",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								{
									
								}
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										{
											
										}
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[{}, {}]",
					"[[ {} ], [ {} ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 17. Find filter
```json
{  "$expr" : {  "$in" : [ "a", "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ "a", "b", "c" ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : "a" }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "C60A7B3F17AA0349C2C5CA6064681B07E3BD32308B096309ED9F4612285A8612",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : "a"
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ "a", "b", "c" ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : "a" } }, { "$expr" : { "$in" : [ { "$const" : "a" }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "C60A7B3F17AA0349C2C5CA6064681B07E3BD32308B096309ED9F4612285A8612",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : "a"
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[\"a\", \"a\"]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 18. Find filter
```json
{  "$expr" : {  "$in" : [ "ab", "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : "ab" }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "C60A7B3F17AA0349C2C5CA6064681B07E3BD32308B096309ED9F4612285A8612",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : "ab"
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : "ab" } }, { "$expr" : { "$in" : [ { "$const" : "ab" }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "C60A7B3F17AA0349C2C5CA6064681B07E3BD32308B096309ED9F4612285A8612",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : "ab"
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[\"ab\", \"ab\"]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 19. Find filter
```json
{  "$expr" : {  "$in" : [ "abc", "$m" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ "abc" ] },
 {  "m" : [ "ghi", "abc", "def" ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : "abc" }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "C60A7B3F17AA0349C2C5CA6064681B07E3BD32308B096309ED9F4612285A8612",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : "abc"
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ "abc" ] },
 {  "m" : [ "ghi", "abc", "def" ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : "abc" } }, { "$expr" : { "$in" : [ { "$const" : "abc" }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "C60A7B3F17AA0349C2C5CA6064681B07E3BD32308B096309ED9F4612285A8612",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : "abc"
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[\"abc\", \"abc\"]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 20. Find filter
```json
{  "$expr" : {  "$in" : [ [ "a" ], "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ "a" ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "7D05D020DEEC3E03C1F750497AE04FB3191B8E8A9ABE3E3B386C29F75089378D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								"a"
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ "a" ] } }, { "$expr" : { "$in" : [ { "$const" : [ "a" ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "7D05D020DEEC3E03C1F750497AE04FB3191B8E8A9ABE3E3B386C29F75089378D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								"a"
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										"a"
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[\"a\", \"a\"]",
					"[[ \"a\" ], [ \"a\" ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 21. Find filter
```json
{  "$expr" : {  "$in" : [ [ "ab" ], "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ "ab" ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "7D05D020DEEC3E03C1F750497AE04FB3191B8E8A9ABE3E3B386C29F75089378D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								"ab"
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ "ab" ] } }, { "$expr" : { "$in" : [ { "$const" : [ "ab" ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "7D05D020DEEC3E03C1F750497AE04FB3191B8E8A9ABE3E3B386C29F75089378D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								"ab"
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										"ab"
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[\"ab\", \"ab\"]",
					"[[ \"ab\" ], [ \"ab\" ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 22. Find filter
```json
{  "$expr" : {  "$in" : [ [ "abc" ], "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ "abc" ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "7D05D020DEEC3E03C1F750497AE04FB3191B8E8A9ABE3E3B386C29F75089378D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								"abc"
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ "abc" ] } }, { "$expr" : { "$in" : [ { "$const" : [ "abc" ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "7D05D020DEEC3E03C1F750497AE04FB3191B8E8A9ABE3E3B386C29F75089378D",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								"abc"
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										"abc"
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[\"abc\", \"abc\"]",
					"[[ \"abc\" ], [ \"abc\" ]]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 23. Find filter
```json
{  "$expr" : {  "$in" : [ [ /a/ ], "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ /a/ ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "4E434920FEE6C5E0D37BE160FDA56E4BD63EDEEB852292F227E4C7C16C8F7F4C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								/a/
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ /a/ ] } }, { "$expr" : { "$in" : [ { "$const" : [ /a/ ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "4E434920FEE6C5E0D37BE160FDA56E4BD63EDEEB852292F227E4C7C16C8F7F4C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								/a/
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										/a/
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[[ /a/ ], [ /a/ ]]",
					"[/a/, /a/]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 24. Find filter
```json
{  "$expr" : {  "$in" : [ [ /b/ ], "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ /b/ ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "4E434920FEE6C5E0D37BE160FDA56E4BD63EDEEB852292F227E4C7C16C8F7F4C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								/b/
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ /b/ ] } }, { "$expr" : { "$in" : [ { "$const" : [ /b/ ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "4E434920FEE6C5E0D37BE160FDA56E4BD63EDEEB852292F227E4C7C16C8F7F4C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								/b/
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										/b/
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[[ /b/ ], [ /b/ ]]",
					"[/b/, /b/]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 25. Find filter
```json
{  "$expr" : {  "$in" : [ [ /abc/ ], "$m" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ /abc/ ] }, "$m" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "4E434920FEE6C5E0D37BE160FDA56E4BD63EDEEB852292F227E4C7C16C8F7F4C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								/abc/
							]
						},
						"$m"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : [ /abc/ ] } }, { "$expr" : { "$in" : [ { "$const" : [ /abc/ ] }, "$m" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "4E434920FEE6C5E0D37BE160FDA56E4BD63EDEEB852292F227E4C7C16C8F7F4C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m" : {
							"$eq" : [
								/abc/
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										/abc/
									]
								},
								"$m"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[[ /abc/ ], [ /abc/ ]]",
					"[/abc/, /abc/]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 26. Find filter
```json
{  "$expr" : {  "$or" : [ { "$in" : [ 1, "$m" ] } ] } }
```
### Query knob off
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] }]
```
Parsed find query
```json
{  "$expr" : {  "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] } ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "C7E2960ED3A09B80F9E47AD2BC5E296B36B5AB8D2BC411CCE417E6BD447F2B5A",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$or" : [
						{
							"$in" : [
								{
									"$const" : 1
								},
								"$m"
							]
						}
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$eq" : 1 } }, { "$expr" : { "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] } ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "C7E2960ED3A09B80F9E47AD2BC5E296B36B5AB8D2BC411CCE417E6BD447F2B5A",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$or" : [
						{
							"$in" : [
								{
									"$const" : 1
								},
								"$m"
							]
						}
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[1.0, 1.0]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 27. Find filter
```json
{  "$expr" : {  "$or" : [ { "$in" : [ 1, "$m" ] }, { "$in" : [ 2, "$m" ] } ] } }
```
### Query knob off
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 2 ] },
 {  "m" : [ 5, 2, 3, 6 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] }]
```
Parsed find query
```json
{  "$expr" : {  "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] } ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "649EE30DE7B8137679D8E9C41B59298BE04B89D2DD2D2A56178D9DED038D39DB",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$or" : [
						{
							"$in" : [
								{
									"$const" : 1
								},
								"$m"
							]
						},
						{
							"$in" : [
								{
									"$const" : 2
								},
								"$m"
							]
						}
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] },
 {  "m" : [ 2 ] },
 {  "m" : [ 5, 2, 3, 6 ] }]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$in" : [ 1, 2 ] } }, { "$expr" : { "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] } ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "649EE30DE7B8137679D8E9C41B59298BE04B89D2DD2D2A56178D9DED038D39DB",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$or" : [
						{
							"$in" : [
								{
									"$const" : 1
								},
								"$m"
							]
						},
						{
							"$in" : [
								{
									"$const" : 2
								},
								"$m"
							]
						}
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[1.0, 1.0]",
					"[2.0, 2.0]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 28. Find filter
```json
{  "$expr" : {  "$or" : [ { "$in" : [ 1, "$m" ] }, { "$in" : [ 2, "$m" ] }, { "$in" : [ "$a", [ 1, 2, 10 ] ] } ] } }
```
### Query knob off
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 2 ] },
 {  "m" : [ 5, 2, 3, 6 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] }]
```
Parsed find query
```json
{  "$expr" : {  "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] }, { "$in" : [ "$a", { "$const" : [ 1, 2, 10 ] } ] } ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "9C0A7A0C8D2673076A90E9E83FF4F8CFDEE00672F25140801334DF24E5E2E7C1",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$or" : [
						{
							"$in" : [
								{
									"$const" : 1
								},
								"$m"
							]
						},
						{
							"$in" : [
								{
									"$const" : 2
								},
								"$m"
							]
						},
						{
							"$in" : [
								"$a",
								{
									"$const" : [
										1,
										2,
										10
									]
								}
							]
						}
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] },
 {  "m" : [ 1, 2 ] },
 {  "m" : [ 2 ] },
 {  "m" : [ 5, 2, 3, 6 ] },
 {  "m" : [ 5, 2, 1, 3, 6 ] }]
```
Parsed find query
```json
{  "$and" : [ { "$or" : [ { "a" : { "$in" : [ 1, 2, 10 ] } }, { "m" : { "$in" : [ 1, 2 ] } } ] }, { "$expr" : { "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] }, { "$in" : [ "$a", { "$const" : [ 1, 2, 10 ] } ] } ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "9C0A7A0C8D2673076A90E9E83FF4F8CFDEE00672F25140801334DF24E5E2E7C1",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$and" : [
					{
						"$or" : [
							{
								"a" : {
									"$in" : [
										1,
										2,
										10
									]
								}
							},
							{
								"m" : {
									"$in" : [
										1,
										2
									]
								}
							}
						]
					},
					{
						"$expr" : {
							"$or" : [
								{
									"$in" : [
										{
											"$const" : 1
										},
										"$m"
									]
								},
								{
									"$in" : [
										{
											"$const" : 2
										},
										"$m"
									]
								},
								{
									"$in" : [
										"$a",
										{
											"$const" : [
												1,
												2,
												10
											]
										}
									]
								}
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

## 29. Find filter
```json
{  "$expr" : {  "$and" : [ { "$in" : [ "$a", [ 1, 2 ] ] }, { "$or" : [ { "$in" : [ 1, "$m" ] }, { "$in" : [ 2, "$m" ] } ] } ] } }
```
### Query knob off
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] }]
```
Parsed find query
```json
{  "$and" : [ { "a" : { "$in" : [ 1, 2 ] } }, { "$expr" : { "$and" : [ { "$in" : [ "$a", { "$const" : [ 1, 2 ] } ] }, { "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] } ] } ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "02049209D21816652EEA49F737BDED51E05D4F01D82B0380B28247FD42526FD2",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$and" : [
					{
						"a" : {
							"$in" : [
								1,
								2
							]
						}
					},
					{
						"$expr" : {
							"$and" : [
								{
									"$in" : [
										"$a",
										{
											"$const" : [
												1,
												2
											]
										}
									]
								},
								{
									"$or" : [
										{
											"$in" : [
												{
													"$const" : 1
												},
												"$m"
											]
										},
										{
											"$in" : [
												{
													"$const" : 2
												},
												"$m"
											]
										}
									]
								}
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "a" : 1,  "m" : [ 1 ] },
 {  "a" : 2,  "m" : [ 1, 2, 3 ] }]
```
Parsed find query
```json
{  "$and" : [ { "a" : { "$in" : [ 1, 2 ] } }, { "m" : { "$in" : [ 1, 2 ] } }, { "$expr" : { "$and" : [ { "$in" : [ "$a", { "$const" : [ 1, 2 ] } ] }, { "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] } ] } ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "02049209D21816652EEA49F737BDED51E05D4F01D82B0380B28247FD42526FD2",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"$expr" : {
							"$and" : [
								{
									"$in" : [
										"$a",
										{
											"$const" : [
												1,
												2
											]
										}
									]
								},
								{
									"$or" : [
										{
											"$in" : [
												{
													"$const" : 1
												},
												"$m"
											]
										},
										{
											"$in" : [
												{
													"$const" : 2
												},
												"$m"
											]
										}
									]
								}
							]
						}
					},
					{
						"a" : {
							"$in" : [
								1,
								2
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[1.0, 1.0]",
					"[2.0, 2.0]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 30. Find filter
```json
{  "$expr" : {  "$and" : [ { "$or" : [ { "$in" : [ 1, "$m" ] }, { "$in" : [ 2, "$m" ] } ] }, { "$in" : [ "$a", [ null ] ] } ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$and" : [ { "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] } ] }, { "$in" : [ "$a", { "$const" : [ null ] } ] } ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "A94B37ED5EF2533D573F31F9FDB1170898925CC71904D271D7B476F9E10CFC7B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$and" : [
						{
							"$or" : [
								{
									"$in" : [
										{
											"$const" : 1
										},
										"$m"
									]
								},
								{
									"$in" : [
										{
											"$const" : 2
										},
										"$m"
									]
								}
							]
						},
						{
							"$in" : [
								"$a",
								{
									"$const" : [
										null
									]
								}
							]
						}
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m" : { "$in" : [ 1, 2 ] } }, { "$expr" : { "$and" : [ { "$or" : [ { "$in" : [ { "$const" : 1 }, "$m" ] }, { "$in" : [ { "$const" : 2 }, "$m" ] } ] }, { "$in" : [ "$a", { "$const" : [ null ] } ] } ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "A94B37ED5EF2533D573F31F9FDB1170898925CC71904D271D7B476F9E10CFC7B",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$and" : [
						{
							"$or" : [
								{
									"$in" : [
										{
											"$const" : 1
										},
										"$m"
									]
								},
								{
									"$in" : [
										{
											"$const" : 2
										},
										"$m"
									]
								}
							]
						},
						{
							"$in" : [
								"$a",
								{
									"$const" : [
										null
									]
								}
							]
						}
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m" : [
					"[1.0, 1.0]",
					"[2.0, 2.0]"
				]
			},
			"indexName" : "m_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m" : 1
			},
			"multiKeyPaths" : {
				"m" : [
					"m"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 31. Find filter
```json
{  "$expr" : {  "$in" : [ 1, "$m.a" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ { "a" : 1 } ] },
 {  "m" : [ { "a" : 1, "b" : 1 } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : 1 }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "23EA1E58A3F112270261BB54458434C39A324D3F7E831277472C2E3001282FBA",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : 1
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ { "a" : 1 } ] },
 {  "m" : [ { "a" : 1, "b" : 1 } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : 1 } }, { "$expr" : { "$in" : [ { "$const" : 1 }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "23EA1E58A3F112270261BB54458434C39A324D3F7E831277472C2E3001282FBA",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : 1
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[1.0, 1.0]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 32. Find filter
```json
{  "$expr" : {  "$in" : [ [ 1 ], "$m.a" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ { "a" : [ 1 ] } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ 1 ] }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "20853CA092BAC00145662EF16814D92D43E7304158D3E8754EBEE4AE441D4817",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								1
							]
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ { "a" : [ 1 ] } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : [ 1 ] } }, { "$expr" : { "$in" : [ { "$const" : [ 1 ] }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "20853CA092BAC00145662EF16814D92D43E7304158D3E8754EBEE4AE441D4817",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m.a" : {
							"$eq" : [
								1
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										1
									]
								},
								"$m.a"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[1.0, 1.0]",
					"[[ 1.0 ], [ 1.0 ]]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 33. Find filter
```json
{  "$expr" : {  "$in" : [ [ [ 1 ] ], "$m.a" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ [ 1 ] ] }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "06EC1906447DEF972175B8516961DAADD80751362222A5B94B3E3AD320B7595E",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								[
									1
								]
							]
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : [ [ 1 ] ] } }, { "$expr" : { "$in" : [ { "$const" : [ [ 1 ] ] }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "06EC1906447DEF972175B8516961DAADD80751362222A5B94B3E3AD320B7595E",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m.a" : {
							"$eq" : [
								[
									1
								]
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										[
											1
										]
									]
								},
								"$m.a"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[[ 1.0 ], [ 1.0 ]]",
					"[[ [ 1.0 ] ], [ [ 1.0 ] ]]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 34. Find filter
```json
{  "$expr" : {  "$in" : [ [ ], "$m.a" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ { "a" : [ ] } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ ] }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "6809C0B2D45BFA79B2D940382D1095FF946EC0C683F97E7F040C3335826AFF4A",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [ ]
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ { "a" : [ ] } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : [ ] } }, { "$expr" : { "$in" : [ { "$const" : [ ] }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "6809C0B2D45BFA79B2D940382D1095FF946EC0C683F97E7F040C3335826AFF4A",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m.a" : {
							"$eq" : [ ]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [ ]
								},
								"$m.a"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[undefined, undefined]",
					"[[], []]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 35. Find filter
```json
{  "$expr" : {  "$in" : [ [ [ ] ], "$m.a" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ [ ] ] }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "06EC1906447DEF972175B8516961DAADD80751362222A5B94B3E3AD320B7595E",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								[ ]
							]
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : [ [ ] ] } }, { "$expr" : { "$in" : [ { "$const" : [ [ ] ] }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "06EC1906447DEF972175B8516961DAADD80751362222A5B94B3E3AD320B7595E",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m.a" : {
							"$eq" : [
								[ ]
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										[ ]
									]
								},
								"$m.a"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[[], []]",
					"[[ [] ], [ [] ]]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 36. Find filter
```json
{  "$expr" : {  "$in" : [ {  }, "$m.a" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ { "a" : {  } } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : {  } }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "EB5E5180527422DE493C24A60B7BB619D462BE97F9AC9B30B2A2D7CAF9E06610",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								
							}
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ { "a" : {  } } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : {  } } }, { "$expr" : { "$in" : [ { "$const" : {  } }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "EB5E5180527422DE493C24A60B7BB619D462BE97F9AC9B30B2A2D7CAF9E06610",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : {
								
							}
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[{}, {}]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 37. Find filter
```json
{  "$expr" : {  "$in" : [ [ {  } ], "$m.a" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ {  } ] }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "BBBE6737A2D98A86E37E177D1089D187FFB1B07680E76AF4BEFAA65C892AA2B1",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								{
									
								}
							]
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : [ {  } ] } }, { "$expr" : { "$in" : [ { "$const" : [ {  } ] }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "BBBE6737A2D98A86E37E177D1089D187FFB1B07680E76AF4BEFAA65C892AA2B1",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m.a" : {
							"$eq" : [
								{
									
								}
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										{
											
										}
									]
								},
								"$m.a"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[{}, {}]",
					"[[ {} ], [ {} ]]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 38. Find filter
```json
{  "$expr" : {  "$in" : [ null, "$m.a" ] } }
```
### Query knob off
Find results
```json
[{  "m" : [ { "a" : null } ] }]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : null }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "87FFBEBB3C0FBD3A2C2DC37B0155C62478505DBCD617008CABB295FA4CA76937",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : null
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[{  "m" : [ { "a" : null } ] }]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : null } }, { "$expr" : { "$in" : [ { "$const" : null }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "87FFBEBB3C0FBD3A2C2DC37B0155C62478505DBCD617008CABB295FA4CA76937",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m.a" : {
							"$eq" : null
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : null
								},
								"$m.a"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[null, null]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 39. Find filter
```json
{  "$expr" : {  "$in" : [ [ null ], "$m.a" ] } }
```
### Query knob off
Find results
```json
[]
```
Parsed find query
```json
{  "$expr" : {  "$in" : [ { "$const" : [ null ] }, "$m.a" ] } }
```
Summarized explain
```json
{
	"queryShapeHash" : "7F670B17E8D47B23C6890CF28FF7373AA10EAB724B0F9599641ED482A9C6B778",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$expr" : {
					"$in" : [
						{
							"$const" : [
								null
							]
						},
						"$m.a"
					]
				}
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

### Query knob on
Find results
```json
[]
```
Parsed find query
```json
{  "$and" : [ { "m.a" : { "$eq" : [ null ] } }, { "$expr" : { "$in" : [ { "$const" : [ null ] }, "$m.a" ] } } ] }
```
Summarized explain
```json
{
	"queryShapeHash" : "7F670B17E8D47B23C6890CF28FF7373AA10EAB724B0F9599641ED482A9C6B778",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"m.a" : {
							"$eq" : [
								null
							]
						}
					},
					{
						"$expr" : {
							"$in" : [
								{
									"$const" : [
										null
									]
								},
								"$m.a"
							]
						}
					}
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"m.a" : [
					"[null, null]",
					"[[ null ], [ null ]]"
				]
			},
			"indexName" : "m.a_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"m.a" : 1
			},
			"multiKeyPaths" : {
				"m.a" : [
					"m",
					"m.a"
				]
			},
			"nss" : "test.expr_in_rewrite_md",
			"stage" : "IXSCAN"
		}
	]
}
```

