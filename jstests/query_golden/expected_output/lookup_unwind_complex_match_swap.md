## 1. Inserting docs into collection "a":
```json
[
	{
		"_id" : 1,
		"b" : 4,
		"my_id" : 100,
		"m" : {
			"c" : 42
		}
	},
	{
		"_id" : 2,
		"b" : 4,
		"my_id" : 101,
		"m" : {
			
		}
	},
	{
		"_id" : 3,
		"b" : 4,
		"my_id" : 100
	},
	{
		"_id" : 4,
		"b" : 4,
		"m" : {
			"c" : null
		}
	},
	{
		"_id" : 5,
		"b" : 4,
		"m" : {
			"c" : 42,
			"d" : "foo"
		}
	}
]
```
## 2. Inserting docs into collection "b":
```json
[
	{
		"_id" : 1,
		"b" : 4,
		"indicator" : "X"
	},
	{
		"_id" : 2,
		"b" : 4,
		"indicator" : "Y"
	},
	{
		"_id" : 3,
		"b" : 4
	},
	{
		"_id" : 4,
		"b" : 4,
		"indicator" : {
			"Z" : "Y"
		}
	},
	{
		"_id" : 5,
		"b" : 4,
		"indicator" : "Z"
	}
]
```
## 3. Inserting docs into collection "c":
```json
[
	{
		"_id" : 1,
		"b" : 4,
		"code" : "X"
	},
	{
		"_id" : 2,
		"b" : 4,
		"other_id" : 42,
		"code" : "bar"
	},
	{
		"_id" : 3,
		"b" : 4,
		"other_id" : 42
	},
	{
		"_id" : 4,
		"b" : 4,
		"code" : "blah"
	},
	{
		"_id" : 5,
		"b" : 4,
		"other_id" : 20,
		"code" : "foo"
	},
	{
		"_id" : 6,
		"b" : 4,
		"other_id" : {
			"zip" : 42,
			"zap" : 20
		},
		"code" : "bar"
	},
	{
		"_id" : 7,
		"b" : 4,
		"other_id" : {
			"zip" : 20,
			"zap" : 42
		}
	}
]
```
## 4. View pipeline
```json
[
	{
		"$match" : {
			"my_id" : 100
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$B_data"
	},
	{
		"$match" : {
			"B_data.indicator" : "Y"
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$C_data"
	},
	{
		"$addFields" : {
			"other_id" : "$C_data.other_id"
		}
	}
]
```
### Query
```json
{  "other_id" : 42 }
```
### Results
```json
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 2,  "b" : 4,  "code" : "bar",  "other_id" : 42 },  "_id" : 1,  "b" : 4,  "m" : {  "c" : 42 },  "my_id" : 100,  "other_id" : 42 }
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 2,  "b" : 4,  "code" : "bar",  "other_id" : 42 },  "_id" : 3,  "b" : 4,  "my_id" : 100,  "other_id" : 42 }
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 3,  "b" : 4,  "other_id" : 42 },  "_id" : 1,  "b" : 4,  "m" : {  "c" : 42 },  "my_id" : 100,  "other_id" : 42 }
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 3,  "b" : 4,  "other_id" : 42 },  "_id" : 3,  "b" : 4,  "my_id" : 100,  "other_id" : 42 }
```
### Explain
```json
[
	{
		"$match" : {
			"my_id" : {
				"$eq" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"indicator" : {
							"$eq" : "Y"
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
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"other_id" : {
							"$eq" : 42
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
		"$addFields" : {
			"other_id" : "$C_data.other_id"
		}
	}
]
```

### Query
```json
{  "other_id.zip" : 42 }
```
### Results
```json
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 6,  "b" : 4,  "code" : "bar",  "other_id" : {  "zap" : 20,  "zip" : 42 } },  "_id" : 1,  "b" : 4,  "m" : {  "c" : 42 },  "my_id" : 100,  "other_id" : {  "zap" : 20,  "zip" : 42 } }
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 6,  "b" : 4,  "code" : "bar",  "other_id" : {  "zap" : 20,  "zip" : 42 } },  "_id" : 3,  "b" : 4,  "my_id" : 100,  "other_id" : {  "zap" : 20,  "zip" : 42 } }
```
### Explain
```json
[
	{
		"$match" : {
			"my_id" : {
				"$eq" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"indicator" : {
							"$eq" : "Y"
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
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"other_id.zip" : {
							"$eq" : 42
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
		"$addFields" : {
			"other_id" : "$C_data.other_id"
		}
	}
]
```

## 5. View pipeline
```json
[
	{
		"$match" : {
			"my_id" : 100
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$B_data"
	},
	{
		"$match" : {
			"B_data.indicator" : "Y"
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$C_data"
	},
	{
		"$addFields" : {
			"zip" : "$C_data.other_id.zip"
		}
	}
]
```
### Query
```json
{  "zip" : 42 }
```
### Results
```json
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 6,  "b" : 4,  "code" : "bar",  "other_id" : {  "zap" : 20,  "zip" : 42 } },  "_id" : 1,  "b" : 4,  "m" : {  "c" : 42 },  "my_id" : 100,  "zip" : 42 }
{  "B_data" : {  "_id" : 2,  "b" : 4,  "indicator" : "Y" },  "C_data" : {  "_id" : 6,  "b" : 4,  "code" : "bar",  "other_id" : {  "zap" : 20,  "zip" : 42 } },  "_id" : 3,  "b" : 4,  "my_id" : 100,  "zip" : 42 }
```
### Explain
```json
[
	{
		"$match" : {
			"my_id" : {
				"$eq" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"indicator" : {
							"$eq" : "Y"
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
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b",
			"unwinding" : {
				"preserveNullAndEmptyArrays" : false
			}
		}
	},
	{
		"$addFields" : {
			"zip" : "$C_data.other_id.zip"
		}
	},
	{
		"$match" : {
			"zip" : {
				"$eq" : 42
			}
		}
	}
]
```

## 6. View pipeline
```json
[
	{
		"$match" : {
			"my_id" : 100
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$B_data"
	},
	{
		"$match" : {
			"B_data.indicator" : "Y"
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$C_data"
	},
	{
		"$project" : {
			"_id" : 1,
			"other_id" : "$C_data.other_id",
			"code" : 1
		}
	}
]
```
### Query
```json
{  "other_id" : 42 }
```
### Results
```json
{  "_id" : 1,  "other_id" : 42 }
{  "_id" : 1,  "other_id" : 42 }
{  "_id" : 3,  "other_id" : 42 }
{  "_id" : 3,  "other_id" : 42 }
```
### Explain
```json
[
	{
		"$match" : {
			"my_id" : {
				"$eq" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"indicator" : {
							"$eq" : "Y"
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
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"other_id" : {
							"$eq" : 42
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
			"_id" : true,
			"code" : true,
			"other_id" : "$C_data.other_id"
		}
	}
]
```

### Query
```json
{  "other_id.zip" : 42 }
```
### Results
```json
{  "_id" : 1,  "other_id" : {  "zap" : 20,  "zip" : 42 } }
{  "_id" : 3,  "other_id" : {  "zap" : 20,  "zip" : 42 } }
```
### Explain
```json
[
	{
		"$match" : {
			"my_id" : {
				"$eq" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"indicator" : {
							"$eq" : "Y"
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
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"other_id.zip" : {
							"$eq" : 42
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
			"_id" : true,
			"code" : true,
			"other_id" : "$C_data.other_id"
		}
	}
]
```

## 7. View pipeline
```json
[
	{
		"$match" : {
			"my_id" : 100
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$B_data"
	},
	{
		"$match" : {
			"B_data.indicator" : "Y"
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$C_data"
	},
	{
		"$project" : {
			"_id" : 1,
			"zip" : "$C_data.other_id.zip",
			"code" : 1
		}
	}
]
```
### Query
```json
{  "zip" : 42 }
```
### Results
```json
{  "_id" : 1,  "zip" : 42 }
{  "_id" : 3,  "zip" : 42 }
```
### Explain
```json
[
	{
		"$match" : {
			"my_id" : {
				"$eq" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"indicator" : {
							"$eq" : "Y"
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
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b",
			"unwinding" : {
				"preserveNullAndEmptyArrays" : false
			}
		}
	},
	{
		"$project" : {
			"_id" : true,
			"code" : true,
			"zip" : "$C_data.other_id.zip"
		}
	},
	{
		"$match" : {
			"zip" : {
				"$eq" : 42
			}
		}
	}
]
```

## 8. View pipeline
```json
[
	{
		"$match" : {
			"my_id" : 100
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$B_data"
	},
	{
		"$match" : {
			"B_data.indicator" : "Y"
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$C_data"
	},
	{
		"$project" : {
			"_id" : 0,
			"indicator" : "$B_data.indicator",
			"code" : "$C_data.code"
		}
	}
]
```
### Query
```json
{  "indicator.Z" : "Y" }
```
### Results
```json

```
### Explain
```json
[
	{
		"$match" : {
			"my_id" : {
				"$eq" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "lu_complex_swap_b",
			"as" : "B_data",
			"localField" : "b",
			"foreignField" : "b",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$match" : {
						"$and" : [
							{
								"indicator" : {
									"$eq" : "Y"
								}
							},
							{
								"indicator.Z" : {
									"$eq" : "Y"
								}
							}
						]
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
			"from" : "lu_complex_swap_c",
			"as" : "C_data",
			"localField" : "b",
			"foreignField" : "b",
			"unwinding" : {
				"preserveNullAndEmptyArrays" : false
			}
		}
	},
	{
		"$project" : {
			"indicator" : "$B_data.indicator",
			"code" : "$C_data.code",
			"_id" : false
		}
	}
]
```

