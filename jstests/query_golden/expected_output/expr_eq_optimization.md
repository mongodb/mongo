## 1. Inserted documents
```json
[
	{
		"a" : 0
	},
	{
		"a" : 1,
		"b" : 1,
		"multiKeyField" : [
			1,
			5,
			10
		]
	},
	{
		"a" : 2,
		"b" : 10,
		"multiKeyField" : [
			3,
			4,
			5
		]
	},
	{
		"a" : 3,
		"b" : 5,
		"multiKeyField" : [
			1,
			10
		]
	},
	{
		"a" : 4,
		"multiKeyField" : 10
	},
	{
		"a" : null,
		"b" : 5
	}
]
```
## 2. Covered projection without fetch stage
Parsed query
```json
{ "$and" : [ { "$expr" : { "$eq" : [ "$a", { "$const" : 3 } ] } }, { "a" : { "$_internalExprEq" : 3 } } ] }
```
Results
```json
[{ "a" : 3 }]
```
Summarized explain
```json
{
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : false,
				"a" : true
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[3.0, 3.0]"
				]
			},
			"indexName" : "a_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ]
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "IXSCAN"
		}
	]
}
```

Parsed query
```json
{ "$and" : [ { "$expr" : { "$eq" : [ { "$const" : 3 }, "$a" ] } }, { "a" : { "$_internalExprEq" : 3 } } ] }
```
Results
```json
[{ "a" : 3 }]
```
Summarized explain
```json
{
	"winningPlan" : [
		{
			"stage" : "PROJECTION_COVERED",
			"transformBy" : {
				"_id" : false,
				"a" : true
			}
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[3.0, 3.0]"
				]
			},
			"indexName" : "a_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ]
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 3. Index scan and fetch without filter.
Parsed query
```json
{ "$and" : [ { "$expr" : { "$eq" : [ "$a", { "$const" : 3 } ] } }, { "a" : { "$_internalExprEq" : 3 } } ] }
```
Results
```json
[{ "a" : 3, "b" : 5 }]
```
Summarized explain
```json
{
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false,
				"a" : true,
				"b" : true
			}
		},
		{
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[3.0, 3.0]"
				]
			},
			"indexName" : "a_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ]
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 4. Index scan and fetch with filter. The original $expr is kept in the fetch filter.
Parsed query
```json
{ "$and" : [ { "$expr" : { "$eq" : [ "$a", { "$const" : null } ] } }, { "a" : { "$_internalExprEq" : null } } ] }
```
Results
```json
[{ "a" : null }]
```
Summarized explain
```json
{
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false,
				"a" : true
			}
		},
		{
			"filter" : {
				"$and" : [
					{
						"$expr" : {
							"$eq" : [
								"$a",
								{
									"$const" : null
								}
							]
						}
					}
				]
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"[null, null]"
				]
			},
			"indexName" : "a_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ]
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "IXSCAN"
		}
	]
}
```

Parsed query
```json
{ "$and" : [ { "$expr" : { "$gt" : [ "$a", { "$const" : 1 } ] } }, { "a" : { "$_internalExprGt" : 1 } } ] }
```
Results
```json
[{ "a" : 2 },
 { "a" : 3 },
 { "a" : 4 }]
```
Summarized explain
```json
{
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false,
				"a" : true
			}
		},
		{
			"filter" : {
				"$expr" : {
					"$gt" : [
						"$a",
						{
							"$const" : 1
						}
					]
				}
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"a" : [
					"(1.0, MaxKey]"
				]
			},
			"indexName" : "a_1",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"a" : 1
			},
			"multiKeyPaths" : {
				"a" : [ ]
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "IXSCAN"
		}
	]
}
```

## 5. Multikey index: the optimization of $expr is not eligible.
Parsed query
```json
{ "$and" : [ { "$expr" : { "$eq" : [ "$multiKeyField", { "$const" : 10 } ] } }, { "multiKeyField" : { "$_internalExprEq" : 10 } } ] }
```
Results
```json
[{ "multiKeyField" : 10 }]
```
Summarized explain
```json
{
	"winningPlan" : [
		{
			"stage" : "PROJECTION_SIMPLE",
			"transformBy" : {
				"_id" : false,
				"multiKeyField" : true
			}
		},
		{
			"direction" : "forward",
			"filter" : {
				"$and" : [
					{
						"$expr" : {
							"$eq" : [
								"$multiKeyField",
								{
									"$const" : 10
								}
							]
						}
					}
				]
			},
			"nss" : "test.expr_eq_optimization_md",
			"stage" : "COLLSCAN"
		}
	]
}
```

