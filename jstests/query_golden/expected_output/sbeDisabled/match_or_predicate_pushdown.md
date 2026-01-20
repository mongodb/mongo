

[jsTest] ----
[jsTest] Resetting collection. Inserting docs:
[jsTest] ----

{  "_id" : 187,  "a" : NumberInt(0),  "array" : [ ],  "b" : NumberInt(0),  "m" : {  "m1" : NumberInt(0),  "m2" : NumberInt(0) },  "t" : ISODate("1970-01-01T00:00:00Z") }
{  "_id" : 83,  "a" : NumberInt(0),  "array" : "",  "b" : NumberInt(0),  "m" : {  "m1" : NumberInt(0),  "m2" : ISODate("1970-01-01T00:00:00Z") },  "t" : ISODate("1970-01-01T00:00:00Z") }
Collection count: 2


[jsTest] ----
[jsTest] Creating indexes:
[jsTest] ----

{  "array" : 1,  "t" : 1 }
### Pipeline
```json
[
	{
		"$match" : {
			"$or" : [
				{
					"t" : {
						"$exists" : true
					}
				},
				{
					"_id" : 0,
					"a" : 0
				}
			],
			"$and" : [
				{
					"array" : {
						"$nin" : [
							0
						]
					}
				},
				{
					"array" : {
						"$eq" : ""
					}
				}
			]
		}
	}
]
```
### Results
```json
{  "_id" : 83,  "a" : 0,  "array" : "",  "b" : 0,  "m" : {  "m1" : 0,  "m2" : ISODate("1970-01-01T00:00:00Z") },  "t" : ISODate("1970-01-01T00:00:00Z") }
```
### Summarized explain
Execution Engine: classic
```json
{
	"queryShapeHash" : "22F20D87ABD4D959DDE7E39FDB212E091657B9BF7FA2D6EB366103791557D83C",
	"rejectedPlans" : [ ],
	"winningPlan" : [
		{
			"filter" : {
				"$and" : [
					{
						"array" : {
							"$not" : {
								"$eq" : 0
							}
						}
					},
					{
						"array" : {
							"$eq" : ""
						}
					}
				]
			},
			"nss" : "test.or_pred_pushdown_coll",
			"stage" : "FETCH"
		},
		{
			"stage" : "OR"
		},
		{
			"filter" : {
				"t" : {
					"$exists" : true
				}
			},
			"nss" : "test.or_pred_pushdown_coll",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"array" : [
					"[\"\", \"\"]"
				],
				"t" : [
					"[MinKey, MaxKey]"
				]
			},
			"indexName" : "t_1_array_1",
			"isMultiKey" : true,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : false,
			"keyPattern" : {
				"array" : 1,
				"t" : 1
			},
			"multiKeyPaths" : {
				"array" : [
					"array"
				],
				"t" : [ ]
			},
			"nss" : "test.or_pred_pushdown_coll",
			"stage" : "IXSCAN"
		},
		{
			"filter" : {
				"a" : {
					"$eq" : 0
				}
			},
			"nss" : "test.or_pred_pushdown_coll",
			"stage" : "FETCH"
		},
		{
			"direction" : "forward",
			"indexBounds" : {
				"_id" : [
					"[0.0, 0.0]"
				]
			},
			"indexName" : "_id_",
			"isMultiKey" : false,
			"isPartial" : false,
			"isSparse" : false,
			"isUnique" : true,
			"keyPattern" : {
				"_id" : 1
			},
			"nss" : "test.or_pred_pushdown_coll",
			"stage" : "IXSCAN"
		}
	]
}
```

