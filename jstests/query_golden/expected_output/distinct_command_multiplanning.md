## 1. No DISTINCT_SCAN candidate considered
### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "z" : 5 }
### Expected results
`[ 5, 6, 7 ]`
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(3.0, inf.0]"
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

## 2. Only DISTINCT_SCAN candidates considered
### Distinct on "x", with filter: { "x" : { "$gt" : 3 }, "y" : 5 }
### Expected results
`[ 5, 6, 7 ]`
### Distinct results
`[ 5, 6, 7 ]`
### Summarized explain
```json
{
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
						"(3.0, inf.0]"
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
						"(3.0, inf.0]"
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
					"(3.0, inf.0]"
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

## 3. Prefer DISTINCT_SCAN for many duplicate values in the collection
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 250 } }
### Expected results
`[ 0, 1 ]`
### Distinct results
`[ 0, 1 ]`
### Summarized explain
```json
{
	"rejectedPlans" : [
		[
			{
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(-1.0, inf.0]"
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
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"y" : [
						"[-inf.0, 250.0)"
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
					"(-1.0, inf.0]"
				],
				"y" : [
					"[-inf.0, 250.0)"
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

## 4. Prefer IXSCAN for no duplicate values in the collection
### Distinct on "x", with filter: { "x" : { "$gt" : -1 }, "y" : { "$lt" : 105 } }
### Expected results
`[ 0, 1, 2, 3, 4 ]`
### Distinct results
`[ 0, 1, 2, 3, 4 ]`
### Summarized explain
```json
{
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
						"(-1.0, inf.0]"
					],
					"y" : [
						"[-inf.0, 105.0)"
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
				"stage" : "FETCH"
			},
			{
				"direction" : "forward",
				"indexBounds" : {
					"x" : [
						"(-1.0, inf.0]"
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
			"stage" : "FETCH"
		},
		{
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

