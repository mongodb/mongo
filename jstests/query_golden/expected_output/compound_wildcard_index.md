## 1. Test a filter using only the wildcard prefix of the CWI
### Query
```json
{ "sub.num" : { "$gt" : 4 } }
```
### Results
```json
{  "_id" : 0,  "num" : 1,  "sub" : {  "num" : 5 } }
{  "_id" : 1,  "num" : 100,  "sub" : {  "num" : 5 } }
{  "_id" : 11,  "str" : "1",  "sub" : {  "num" : 9 } }
{  "_id" : 12,  "str" : "not_matching",  "sub" : {  "num" : 9 } }
{  "_id" : 13,  "str" : "1",  "sub" : {  "num" : 11 } }
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "sub.$**_1_num_1",
		"indexBounds" : {
			"$_path" : [
				"[\"sub.num\", \"sub.num\"]"
			],
			"sub.num" : [
				"(4.0, inf]"
			],
			"num" : [
				"[MinKey, MaxKey]"
			]
		},
		"keyPattern" : {
			"$_path" : 1,
			"sub.num" : 1,
			"num" : 1
		}
	}
]
```

## 2. Test a filter using both components of the CWI
### Query
```json
{ "num" : 1, "sub.num" : { "$gt" : 4 } }
```
### Results
```json
{  "_id" : 0,  "num" : 1,  "sub" : {  "num" : 5 } }
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "sub.$**_1_num_1",
		"indexBounds" : {
			"$_path" : [
				"[\"sub.num\", \"sub.num\"]"
			],
			"sub.num" : [
				"(4.0, inf]"
			],
			"num" : [
				"[1.0, 1.0]"
			]
		},
		"keyPattern" : {
			"$_path" : 1,
			"sub.num" : 1,
			"num" : 1
		}
	}
]
```

## 3. Test an $elemMatch (object) filter using both components of the CWI
### Query
```json
{
	"num" : 1,
	"sub" : {
		"$elemMatch" : {
			"num" : {
				"$gt" : 4
			}
		}
	}
}
```
### Results
```json
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
```
### Index Scans
```json
[
	{
		"indexName" : "sub.$**_1_num_1",
		"indexBounds" : {
			"$_path" : [
				"[\"sub.num\", \"sub.num\"]"
			],
			"sub.num" : [
				"(4.0, inf]"
			],
			"num" : [
				"[1.0, 1.0]"
			]
		},
		"keyPattern" : {
			"$_path" : 1,
			"sub.num" : 1,
			"num" : 1
		}
	}
]
```

## 4. Test an $elemMatch (value) filter using both components of the CWI
### Query
```json
{ "num" : 1, "sub.num" : { "$elemMatch" : { "$gt" : 4 } } }
```
### Results
```json
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "sub.$**_1_num_1",
		"indexBounds" : {
			"$_path" : [
				"[\"sub.num\", \"sub.num\"]"
			],
			"sub.num" : [
				"(4.0, inf]"
			],
			"num" : [
				"[1.0, 1.0]"
			]
		},
		"keyPattern" : {
			"$_path" : 1,
			"sub.num" : 1,
			"num" : 1
		}
	}
]
```

## 5. Test a filter using only the non-wildcard prefix of the CWI
### Query
```json
{ "num" : 1 }
```
### Results
```json
{  "_id" : 0,  "num" : 1,  "sub" : {  "num" : 5 } }
{  "_id" : 2,  "num" : 1,  "sub" : {  "num" : 0 } }
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
{  "_id" : 4,  "num" : 1,  "sub" : [ { "num" : 0 } ] }
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "num_1_sub.$**_1",
		"indexBounds" : {
			"num" : [
				"[1.0, 1.0]"
			],
			"$_path" : [
				"[MinKey, MinKey]",
				"[\"\", {})"
			]
		},
		"keyPattern" : {
			"num" : 1,
			"$_path" : 1
		}
	}
]
```

## 6. Test a filter using both components of the CWI
### Query
```json
{ "num" : 1, "sub.num" : { "$gt" : 4 } }
```
### Results
```json
{  "_id" : 0,  "num" : 1,  "sub" : {  "num" : 5 } }
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "num_1_sub.$**_1",
		"indexBounds" : {
			"num" : [
				"[1.0, 1.0]"
			],
			"$_path" : [
				"[\"sub.num\", \"sub.num\"]"
			],
			"sub.num" : [
				"(4.0, inf]"
			]
		},
		"keyPattern" : {
			"num" : 1,
			"$_path" : 1,
			"sub.num" : 1
		}
	}
]
```

## 7. Test an $or filter using both components of the CWI
### Query
```json
{
	"$or" : [
		{
			"num" : 1
		},
		{
			"sub.num" : {
				"$gt" : 4
			}
		}
	]
}
```
### Results
```json
{  "_id" : 0,  "num" : 1,  "sub" : {  "num" : 5 } }
{  "_id" : 1,  "num" : 100,  "sub" : {  "num" : 5 } }
{  "_id" : 11,  "str" : "1",  "sub" : {  "num" : 9 } }
{  "_id" : 12,  "str" : "not_matching",  "sub" : {  "num" : 9 } }
{  "_id" : 13,  "str" : "1",  "sub" : {  "num" : 11 } }
{  "_id" : 2,  "num" : 1,  "sub" : {  "num" : 0 } }
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
{  "_id" : 4,  "num" : 1,  "sub" : [ { "num" : 0 } ] }
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "num_1_sub.$**_1",
		"indexBounds" : {
			"num" : [
				"[1.0, 1.0]"
			],
			"$_path" : [
				"[MinKey, MinKey]",
				"[\"\", {})"
			]
		},
		"keyPattern" : {
			"num" : 1,
			"$_path" : 1
		}
	},
	{
		"indexName" : "sub.num_1",
		"indexBounds" : {
			"sub.num" : [
				"(4.0, inf]"
			]
		},
		"keyPattern" : {
			"sub.num" : 1
		}
	}
]
```

## 8. Test a filter with nested $and under a $or
### Query
```json
{
	"$or" : [
		{
			"num" : 1,
			"sub.num" : {
				"$gt" : 4
			}
		},
		{
			"str" : "1",
			"sub.num" : {
				"$lt" : 10
			}
		}
	]
}
```
### Results
```json
{  "_id" : 0,  "num" : 1,  "sub" : {  "num" : 5 } }
{  "_id" : 11,  "str" : "1",  "sub" : {  "num" : 9 } }
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "num_1_sub.$**_1",
		"indexBounds" : {
			"num" : [
				"[1.0, 1.0]"
			],
			"$_path" : [
				"[\"sub.num\", \"sub.num\"]"
			],
			"sub.num" : [
				"(4.0, inf]"
			]
		},
		"keyPattern" : {
			"num" : 1,
			"$_path" : 1,
			"sub.num" : 1
		}
	},
	{
		"indexName" : "str_1_sub.$**_1",
		"indexBounds" : {
			"str" : [
				"[\"1\", \"1\"]"
			],
			"$_path" : [
				"[\"sub.num\", \"sub.num\"]"
			],
			"sub.num" : [
				"[-inf, 10.0)"
			]
		},
		"keyPattern" : {
			"str" : 1,
			"$_path" : 1,
			"sub.num" : 1
		}
	}
]
```

## 9. Test a filter with nested $or under an $and
### Query
```json
{
	"$and" : [
		{
			"$or" : [
				{
					"num" : 1
				},
				{
					"sub.num" : {
						"$gt" : 4
					}
				}
			]
		},
		{
			"$or" : [
				{
					"str" : "1"
				},
				{
					"sub.num" : {
						"$lt" : 10
					}
				}
			]
		}
	]
}
```
### Results
```json
{  "_id" : 0,  "num" : 1,  "sub" : {  "num" : 5 } }
{  "_id" : 1,  "num" : 100,  "sub" : {  "num" : 5 } }
{  "_id" : 11,  "str" : "1",  "sub" : {  "num" : 9 } }
{  "_id" : 12,  "str" : "not_matching",  "sub" : {  "num" : 9 } }
{  "_id" : 13,  "str" : "1",  "sub" : {  "num" : 11 } }
{  "_id" : 2,  "num" : 1,  "sub" : {  "num" : 0 } }
{  "_id" : 3,  "num" : 1,  "sub" : [ { "num" : 5 } ] }
{  "_id" : 4,  "num" : 1,  "sub" : [ { "num" : 0 } ] }
{  "_id" : 5,  "num" : 1,  "sub" : {  "num" : [ 5 ] } }
```
### Index Scans
```json
[
	{
		"indexName" : "str_1_sub.$**_1",
		"indexBounds" : {
			"str" : [
				"[\"1\", \"1\"]"
			],
			"$_path" : [
				"[MinKey, MinKey]",
				"[\"\", {})"
			]
		},
		"keyPattern" : {
			"str" : 1,
			"$_path" : 1
		}
	},
	{
		"indexName" : "sub.num_1",
		"indexBounds" : {
			"sub.num" : [
				"[-inf, 10.0)"
			]
		},
		"keyPattern" : {
			"sub.num" : 1
		}
	}
]
```

## 10. Test the query reproducing SERVER-95374
### Query
```json
{
	"$or" : [
		{
			"$and" : [
				{
					"str" : /myRegex/
				},
				{
					"num" : {
						"$lte" : 0
					}
				}
			]
		},
		{
			"z" : 1
		}
	]
}
```
### Results
```json
{  "_id" : 0,  "num" : 0,  "str" : "myRegex",  "z" : 0 }
{  "_id" : 1,  "num" : 100,  "str" : "not_matching",  "z" : 1 }
{  "_id" : 2,  "num" : 0,  "str" : "not_matching",  "z" : 1 }
{  "_id" : 3,  "num" : 100,  "str" : "myRegex",  "z" : 1 }
```
### Index Scans
```json
[
	{
		"indexName" : "str_1_$**_1",
		"indexBounds" : {
			"str" : [
				"[\"\", {})",
				"[/myRegex/, /myRegex/]"
			],
			"$_path" : [
				"[\"num\", \"num\"]"
			],
			"num" : [
				"[-inf, 0.0]"
			]
		},
		"keyPattern" : {
			"str" : 1,
			"$_path" : 1,
			"num" : 1
		}
	},
	{
		"indexName" : "z_1",
		"indexBounds" : {
			"z" : [
				"[1.0, 1.0]"
			]
		},
		"keyPattern" : {
			"z" : 1
		}
	}
]
```

