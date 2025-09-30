## 1. Inserting docs:
```json
[
	{
		"_id" : 1,
		"z" : 11,
		"h" : {
			"i" : 11
		},
		"b" : {
			"c" : 42
		}
	},
	{
		"_id" : 2,
		"z" : 12,
		"h" : {
			"i" : 12
		},
		"b" : {
			
		}
	},
	{
		"_id" : 3,
		"z" : 13,
		"h" : {
			"i" : 13
		},
		"b" : {
			"c" : null
		}
	},
	{
		"_id" : 4,
		"z" : 14,
		"h" : {
			"i" : 14
		},
		"b" : {
			"c" : 42,
			"d" : "foo"
		}
	},
	{
		"_id" : 5,
		"z" : 15,
		"h" : {
			"i" : 15
		},
		"b" : {
			"c" : {
				"e" : 42,
				"f" : "bar"
			}
		}
	},
	{
		"_id" : 6,
		"z" : 16,
		"h" : {
			"i" : 16
		},
		"b" : {
			"c" : {
				"e" : 42,
				"f" : {
					"g" : 9
				}
			},
			"d" : "foo"
		}
	}
]
```
## 2. Basic inclusion projection
### Pipeline
```json
[ { "$project" : { "_id" : 1, "a" : "$b.c", "z" : 1 } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 1,  "a" : 42,  "z" : 11 }
{  "_id" : 4,  "a" : 42,  "z" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$eq" : 42
			}
		}
	},
	{
		"$project" : {
			"_id" : true,
			"z" : true,
			"a" : "$b.c"
		}
	}
]
```

## 3. Basic inclusion projection with excluded _id (variation 1)
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c", "z" : 1 } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "a" : 42,  "z" : 11 }
{  "a" : 42,  "z" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$eq" : 42
			}
		}
	},
	{
		"$project" : {
			"z" : true,
			"a" : "$b.c",
			"_id" : false
		}
	}
]
```

## 4. Basic inclusion projection with excluded _id (variation 2)
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c" } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "a" : 42 }
{  "a" : 42 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$eq" : 42
			}
		}
	},
	{
		"$project" : {
			"a" : "$b.c",
			"_id" : false
		}
	}
]
```

## 5. Exclusion projection followed by inclusion projection
### Pipeline
```json
[ { "$project" : { "_id" : 0, "z" : 0 } }, { "$project" : { "a" : "$b.c" } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "a" : 42 }
{  "a" : 42 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$eq" : 42
			}
		}
	},
	{
		"$project" : {
			"_id" : false,
			"z" : false
		}
	},
	{
		"$project" : {
			"_id" : true,
			"a" : "$b.c"
		}
	}
]
```

## 6. Basic $addFields
### Pipeline
```json
[ { "$addFields" : { "a" : "$b.c" } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 1,  "a" : 42,  "b" : {  "c" : 42 },  "h" : {  "i" : 11 },  "z" : 11 }
{  "_id" : 4,  "a" : 42,  "b" : {  "c" : 42,  "d" : "foo" },  "h" : {  "i" : 14 },  "z" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$eq" : 42
			}
		}
	},
	{
		"$addFields" : {
			"a" : "$b.c"
		}
	}
]
```

## 7. Basic $set
### Pipeline
```json
[ { "$set" : { "a" : "$b.c" } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 1,  "a" : 42,  "b" : {  "c" : 42 },  "h" : {  "i" : 11 },  "z" : 11 }
{  "_id" : 4,  "a" : 42,  "b" : {  "c" : 42,  "d" : "foo" },  "h" : {  "i" : 14 },  "z" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$eq" : 42
			}
		}
	},
	{
		"$set" : {
			"a" : "$b.c"
		}
	}
]
```

## 8. Inclusion projection with a match on a subpath of the renamed path (variation 1)
### Pipeline
```json
[ { "$project" : { "_id" : 1, "a" : "$b.c", "z" : 1 } }, { "$match" : { "a.e" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 5,  "a" : {  "e" : 42,  "f" : "bar" },  "z" : 15 }
{  "_id" : 6,  "a" : {  "e" : 42,  "f" : {  "g" : 9 } },  "z" : 16 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c.e" : {
				"$eq" : 42
			}
		}
	},
	{
		"$project" : {
			"_id" : true,
			"z" : true,
			"a" : "$b.c"
		}
	}
]
```

## 9. Inclusion projection with a match on a subpath of the renamed path (variation 2)
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c", "z" : 1 } }, { "$match" : { "a.e" : { "$gte" : 42 } } } ]
```
### Results
```json
{  "a" : {  "e" : 42,  "f" : "bar" },  "z" : 15 }
{  "a" : {  "e" : 42,  "f" : {  "g" : 9 } },  "z" : 16 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c.e" : {
				"$gte" : 42
			}
		}
	},
	{
		"$project" : {
			"z" : true,
			"a" : "$b.c",
			"_id" : false
		}
	}
]
```

## 10. Inclusion projection with a match on a subpath of the renamed path (variation 3)
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c" } }, { "$match" : { "a.e" : { "$type" : "number" } } } ]
```
### Results
```json
{  "a" : {  "e" : 42,  "f" : "bar" } }
{  "a" : {  "e" : 42,  "f" : {  "g" : 9 } } }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c.e" : {
				"$type" : [
					"number"
				]
			}
		}
	},
	{
		"$project" : {
			"a" : "$b.c",
			"_id" : false
		}
	}
]
```

## 11. Exclusion/inclusion projection with a match on a subpath of the renamed path
### Pipeline
```json
[ { "$project" : { "_id" : 0, "z" : 0 } }, { "$project" : { "a" : "$b.c" } }, { "$match" : { "a.e" : { "$mod" : [ 7, 0 ] } } } ]
```
### Results
```json
{  "a" : {  "e" : 42,  "f" : "bar" } }
{  "a" : {  "e" : 42,  "f" : {  "g" : 9 } } }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c.e" : {
				"$mod" : [
					NumberLong(7),
					NumberLong(0)
				]
			}
		}
	},
	{
		"$project" : {
			"_id" : false,
			"z" : false
		}
	},
	{
		"$project" : {
			"_id" : true,
			"a" : "$b.c"
		}
	}
]
```

## 12. $addFields with a match on a subpath of the renamed path
### Pipeline
```json
[ { "$addFields" : { "a" : "$b.c" } }, { "$match" : { "a.e" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 5,  "a" : {  "e" : 42,  "f" : "bar" },  "b" : {  "c" : {  "e" : 42,  "f" : "bar" } },  "h" : {  "i" : 15 },  "z" : 15 }
{  "_id" : 6,  "a" : {  "e" : 42,  "f" : {  "g" : 9 } },  "b" : {  "c" : {  "e" : 42,  "f" : {  "g" : 9 } },  "d" : "foo" },  "h" : {  "i" : 16 },  "z" : 16 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c.e" : {
				"$eq" : 42
			}
		}
	},
	{
		"$addFields" : {
			"a" : "$b.c"
		}
	}
]
```

## 13. $set with a match on a subpath of the renamed path
### Pipeline
```json
[ { "$set" : { "a" : "$b.c" } }, { "$match" : { "a.e" : { "$lte" : 42 } } } ]
```
### Results
```json
{  "_id" : 5,  "a" : {  "e" : 42,  "f" : "bar" },  "b" : {  "c" : {  "e" : 42,  "f" : "bar" } },  "h" : {  "i" : 15 },  "z" : 15 }
{  "_id" : 6,  "a" : {  "e" : 42,  "f" : {  "g" : 9 } },  "b" : {  "c" : {  "e" : 42,  "f" : {  "g" : 9 } },  "d" : "foo" },  "h" : {  "i" : 16 },  "z" : 16 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c.e" : {
				"$lte" : 42
			}
		}
	},
	{
		"$set" : {
			"a" : "$b.c"
		}
	}
]
```

## 14. Chain of complex renames
### Pipeline
```json
[ { "$project" : { "_id" : 0, "n" : "$b.c" } }, { "$addFields" : { "q" : "$n.f" } }, { "$set" : { "r" : "$q.g" } }, { "$match" : { "r" : { "$eq" : 9 } } } ]
```
### Results
```json
{  "n" : {  "e" : 42,  "f" : {  "g" : 9 } },  "q" : {  "g" : 9 },  "r" : 9 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c.f.g" : {
				"$eq" : 9
			}
		}
	},
	{
		"$project" : {
			"n" : "$b.c",
			"_id" : false
		}
	},
	{
		"$addFields" : {
			"q" : "$n.f"
		}
	},
	{
		"$set" : {
			"r" : "$q.g"
		}
	}
]
```

## 15. Multiple complex renames
### Pipeline
```json
[ { "$project" : { "n" : "$b.c", "q" : "$h.i" } }, { "$match" : { "$or" : [ { "n" : { "$gt" : 15 } }, { "q" : { "$lt" : 13 } } ] } } ]
```
### Results
```json
{  "_id" : 1,  "n" : 42,  "q" : 11 }
{  "_id" : 2,  "q" : 12 }
{  "_id" : 4,  "n" : 42,  "q" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"b.c" : {
						"$gt" : 15
					}
				},
				{
					"h.i" : {
						"$lt" : 13
					}
				}
			]
		}
	},
	{
		"$project" : {
			"_id" : true,
			"n" : "$b.c",
			"q" : "$h.i"
		}
	}
]
```

## 16. Multiple complex renames as successive pipeline stages
### Pipeline
```json
[ { "$project" : { "n" : "$b.c", "h" : 1 } }, { "$addFields" : { "q" : "$h.i" } }, { "$project" : { "h" : 0 } }, { "$match" : { "$or" : [ { "n" : { "$gt" : 15 } }, { "q" : { "$lt" : 13 } } ] } } ]
```
### Results
```json
{  "_id" : 1,  "n" : 42,  "q" : 11 }
{  "_id" : 2,  "q" : 12 }
{  "_id" : 4,  "n" : 42,  "q" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"b.c" : {
						"$gt" : 15
					}
				},
				{
					"h.i" : {
						"$lt" : 13
					}
				}
			]
		}
	},
	{
		"$project" : {
			"_id" : true,
			"h" : true,
			"n" : "$b.c"
		}
	},
	{
		"$addFields" : {
			"q" : "$h.i"
		}
	},
	{
		"$project" : {
			"h" : false,
			"_id" : true
		}
	}
]
```

## 17. $match swaps past rename due to group
### Pipeline
```json
[ { "$group" : { "_id" : { "z" : "$z" } } }, { "$match" : { "_id.z" : { "$lte" : 14 } } } ]
```
### Results
```json
{  "_id" : {  "z" : 11 } }
{  "_id" : {  "z" : 12 } }
{  "_id" : {  "z" : 13 } }
{  "_id" : {  "z" : 14 } }
```
### Explain
```json
[
	{
		"$match" : {
			"z" : {
				"$lte" : 14
			}
		}
	},
	{
		"$group" : {
			"_id" : {
				"z" : "$z"
			}
		}
	}
]
```

## 18. $match swaps past rename in the presence of arrays created by the pipeline
### Pipeline
```json
[ { "$lookup" : { "from" : "complex_match_swap", "pipeline" : [ { "$group" : { "_id" : "$a", "b" : { "$push" : "$b" } } } ], "as" : "arr" } }, { "$project" : { "c" : "$arr.b" } }, { "$match" : { "c" : { "$eq" : {  } } } } ]
```
### Results
```json
{  "_id" : 1,  "c" : [ [ { "c" : 42 }, {  }, { "c" : null }, { "c" : 42, "d" : "foo" }, { "c" : { "e" : 42, "f" : "bar" } }, { "c" : { "e" : 42, "f" : { "g" : 9 } }, "d" : "foo" } ] ] }
{  "_id" : 2,  "c" : [ [ { "c" : 42 }, {  }, { "c" : null }, { "c" : 42, "d" : "foo" }, { "c" : { "e" : 42, "f" : "bar" } }, { "c" : { "e" : 42, "f" : { "g" : 9 } }, "d" : "foo" } ] ] }
{  "_id" : 3,  "c" : [ [ { "c" : 42 }, {  }, { "c" : null }, { "c" : 42, "d" : "foo" }, { "c" : { "e" : 42, "f" : "bar" } }, { "c" : { "e" : 42, "f" : { "g" : 9 } }, "d" : "foo" } ] ] }
{  "_id" : 4,  "c" : [ [ { "c" : 42 }, {  }, { "c" : null }, { "c" : 42, "d" : "foo" }, { "c" : { "e" : 42, "f" : "bar" } }, { "c" : { "e" : 42, "f" : { "g" : 9 } }, "d" : "foo" } ] ] }
{  "_id" : 5,  "c" : [ [ { "c" : 42 }, {  }, { "c" : null }, { "c" : 42, "d" : "foo" }, { "c" : { "e" : 42, "f" : "bar" } }, { "c" : { "e" : 42, "f" : { "g" : 9 } }, "d" : "foo" } ] ] }
{  "_id" : 6,  "c" : [ [ { "c" : 42 }, {  }, { "c" : null }, { "c" : 42, "d" : "foo" }, { "c" : { "e" : 42, "f" : "bar" } }, { "c" : { "e" : 42, "f" : { "g" : 9 } }, "d" : "foo" } ] ] }
```
### Explain
```json
[
	{
		"$lookup" : {
			"from" : "complex_match_swap",
			"as" : "arr",
			"let" : {
				
			},
			"pipeline" : [
				{
					"$group" : {
						"_id" : "$a",
						"b" : {
							"$push" : "$b"
						}
					}
				}
			]
		}
	},
	{
		"$match" : {
			"arr.b" : {
				"$eq" : {
					
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : true,
			"c" : "$arr.b"
		}
	}
]
```

## 19. $match with $exists swaps past rename
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c", "z" : 1 } }, { "$match" : { "a" : { "$exists" : true } } } ]
```
### Results
```json
{  "a" : 42,  "z" : 11 }
{  "a" : 42,  "z" : 14 }
{  "a" : null,  "z" : 13 }
{  "a" : {  "e" : 42,  "f" : "bar" },  "z" : 15 }
{  "a" : {  "e" : 42,  "f" : {  "g" : 9 } },  "z" : 16 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$exists" : true
			}
		}
	},
	{
		"$project" : {
			"z" : true,
			"a" : "$b.c",
			"_id" : false
		}
	}
]
```

## 20. $match with $expr swaps past rename
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c", "z" : 1 } }, { "$match" : { "$expr" : { "$eq" : [ "$a", 42 ] } } } ]
```
### Results
```json
{  "a" : 42,  "z" : 11 }
{  "a" : 42,  "z" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"b.c" : {
						"$_internalExprEq" : 42
					}
				},
				{
					"$expr" : {
						"$eq" : [
							"$b.c",
							{
								"$const" : 42
							}
						]
					}
				}
			]
		}
	},
	{
		"$project" : {
			"z" : true,
			"a" : "$b.c",
			"_id" : false
		}
	}
]
```

## 21. Dotted path on the left and the right
### Pipeline
```json
[ { "$project" : { "_id" : 0, "x.y" : "$b.c", "z" : 1 } }, { "$match" : { "x.y" : { "$lte" : 42 } } } ]
```
### Results
```json
{  "x" : {  "y" : 42 },  "z" : 11 }
{  "x" : {  "y" : 42 },  "z" : 14 }
```
### Explain
```json
[
	{
		"$match" : {
			"b.c" : {
				"$lte" : 42
			}
		}
	},
	{
		"$project" : {
			"z" : true,
			"x" : {
				"y" : "$b.c"
			},
			"_id" : false
		}
	}
]
```

## 22. Negative case: conditional projection
### Pipeline
```json
[ { "$project" : { "a" : { "$cond" : { "if" : { "$eq" : [ null, "$b.c" ] }, "then" : "$$REMOVE", "else" : "$b.c" } } } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 1,  "a" : 42 }
{  "_id" : 4,  "a" : 42 }
```
### Explain
```json
[
	{
		"$project" : {
			"_id" : true,
			"a" : {
				"$cond" : [
					{
						"$eq" : [
							{
								"$const" : null
							},
							"$b.c"
						]
					},
					"$$REMOVE",
					"$b.c"
				]
			}
		}
	},
	{
		"$match" : {
			"a" : {
				"$eq" : 42
			}
		}
	}
]
```

## 23. Negative case: field path of length 3
### Pipeline
```json
[ { "$project" : { "_id" : 1, "a" : "$b.c.e", "z" : 1 } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 5,  "a" : 42,  "z" : 15 }
{  "_id" : 6,  "a" : 42,  "z" : 16 }
```
### Explain
```json
[
	{
		"$project" : {
			"_id" : true,
			"z" : true,
			"a" : "$b.c.e"
		}
	},
	{
		"$match" : {
			"a" : {
				"$eq" : 42
			}
		}
	}
]
```

## 24. Negative case: field path of length 3 with _id excluded (variation 1)
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c.e", "z" : 1 } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "a" : 42,  "z" : 15 }
{  "a" : 42,  "z" : 16 }
```
### Explain
```json
[
	{
		"$project" : {
			"z" : true,
			"a" : "$b.c.e",
			"_id" : false
		}
	},
	{
		"$match" : {
			"a" : {
				"$eq" : 42
			}
		}
	}
]
```

## 25. Negative case: field path of length 3 with _id excluded (variation 2)
### Pipeline
```json
[ { "$project" : { "_id" : 0, "a" : "$b.c.e" } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "a" : 42 }
{  "a" : 42 }
```
### Explain
```json
[
	{
		"$project" : {
			"a" : "$b.c.e",
			"_id" : false
		}
	},
	{
		"$match" : {
			"a" : {
				"$eq" : 42
			}
		}
	}
]
```

## 26. Negative case: $addFields with field path of length 3
### Pipeline
```json
[ { "$addFields" : { "a" : "$b.c.e" } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 5,  "a" : 42,  "b" : {  "c" : {  "e" : 42,  "f" : "bar" } },  "h" : {  "i" : 15 },  "z" : 15 }
{  "_id" : 6,  "a" : 42,  "b" : {  "c" : {  "e" : 42,  "f" : {  "g" : 9 } },  "d" : "foo" },  "h" : {  "i" : 16 },  "z" : 16 }
```
### Explain
```json
[
	{
		"$addFields" : {
			"a" : "$b.c.e"
		}
	},
	{
		"$match" : {
			"a" : {
				"$eq" : 42
			}
		}
	}
]
```

## 27. Negative case: $set with field path of length 3
### Pipeline
```json
[ { "$set" : { "a" : "$b.c.e" } }, { "$match" : { "a" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "_id" : 5,  "a" : 42,  "b" : {  "c" : {  "e" : 42,  "f" : "bar" } },  "h" : {  "i" : 15 },  "z" : 15 }
{  "_id" : 6,  "a" : 42,  "b" : {  "c" : {  "e" : 42,  "f" : {  "g" : 9 } },  "d" : "foo" },  "h" : {  "i" : 16 },  "z" : 16 }
```
### Explain
```json
[
	{
		"$set" : {
			"a" : "$b.c.e"
		}
	},
	{
		"$match" : {
			"a" : {
				"$eq" : 42
			}
		}
	}
]
```

## 28. Negative case: field path of length 4
### Pipeline
```json
[ { "$project" : { "a" : "$b.c.f.g", "z" : 1 } }, { "$match" : { "a" : { "$eq" : 9 } } } ]
```
### Results
```json
{  "_id" : 6,  "a" : 9,  "z" : 16 }
```
### Explain
```json
[
	{
		"$project" : {
			"_id" : true,
			"z" : true,
			"a" : "$b.c.f.g"
		}
	},
	{
		"$match" : {
			"a" : {
				"$eq" : 9
			}
		}
	}
]
```

## 29. Negative case: $match cannot be pushed beneath $replaceRoot
### Pipeline
```json
[ { "$replaceRoot" : { "newRoot" : "$b" } }, { "$match" : { "c" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "c" : 42 }
{  "c" : 42,  "d" : "foo" }
```
### Explain
```json
[
	{
		"$replaceRoot" : {
			"newRoot" : "$b"
		}
	},
	{
		"$match" : {
			"c" : {
				"$eq" : 42
			}
		}
	}
]
```

## 30. Negative case: $match cannot be pushed beneath $replaceWith
### Pipeline
```json
[ { "$replaceWith" : "$b" }, { "$match" : { "c" : { "$eq" : 42 } } } ]
```
### Results
```json
{  "c" : 42 }
{  "c" : 42,  "d" : "foo" }
```
### Explain
```json
[
	{
		"$replaceRoot" : {
			"newRoot" : "$b"
		}
	},
	{
		"$match" : {
			"c" : {
				"$eq" : 42
			}
		}
	}
]
```

## 31. Negative case: $match cannot swap past complex rename when matching on subfield of $group key
### Pipeline
```json
[ { "$group" : { "_id" : { "x" : "$b.c" } } }, { "$match" : { "_id.x.e" : { "$lte" : 42 } } } ]
```
### Results
```json
{  "_id" : {  "x" : {  "e" : 42,  "f" : "bar" } } }
{  "_id" : {  "x" : {  "e" : 42,  "f" : {  "g" : 9 } } } }
```
### Explain
```json
[
	{
		"$group" : {
			"_id" : {
				"x" : "$b.c"
			}
		}
	},
	{
		"$match" : {
			"_id.x.e" : {
				"$lte" : 42
			}
		}
	}
]
```

