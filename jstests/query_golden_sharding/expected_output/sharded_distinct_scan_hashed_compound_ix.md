## 1. $group by field that is hashed on shard key but not on index
### Pipeline
```json
[ { "$group" : { "_id" : "$a" } } ]
```
### Results
```json
{  "_id" : 0 }
{  "_id" : 0.1 }
{  "_id" : 0.2 }
{  "_id" : 0.9 }
{  "_id" : 1.9 }
```
### Summarized explain
```json
{
	"queryShapeHash" : "D9C9FECEDCF44552BDAE97948C54C49831832F11DC812AF429C85FE39BABE26F",
	"sharded_distinct_scan_hashed_compound_ix-rs0" : [
		{
			"$cursor" : {
				"rejectedPlans" : [ ],
				"winningPlan" : [
					{
						"stage" : "PROJECTION_COVERED",
						"transformBy" : {
							"_id" : 0,
							"a" : 1
						}
					},
					{
						"direction" : "forward",
						"indexBounds" : {
							"a" : [
								"[MinKey, MaxKey]"
							],
							"m" : [
								"[MinKey, MaxKey]"
							]
						},
						"indexName" : "a_1_m_hashed",
						"isFetching" : false,
						"isMultiKey" : false,
						"isPartial" : false,
						"isShardFiltering" : false,
						"isSparse" : false,
						"isUnique" : false,
						"keyPattern" : {
							"a" : 1,
							"m" : "hashed"
						},
						"stage" : "DISTINCT_SCAN"
					}
				]
			}
		},
		{
			"$groupByDistinctScan" : {
				"newRoot" : {
					"_id" : "$a"
				}
			}
		}
	]
}
```

## 2. distinct() on field that is hashed in shard key but in index
### Distinct on "a", with filter: { }
### Distinct results
`[ 0, 0.1, 0.2, 0.9, 1.9 ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SINGLE_SHARD"
		}
	],
	"queryShapeHash" : "0D1E1B86B99E41A2D9F60B1CA65B9CB0BFDC2A31FAFB84E78CF5F8EECB797CF6",
	"shardsPart" : {
		"sharded_distinct_scan_hashed_compound_ix-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"a" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"a" : [
							"[MinKey, MaxKey]"
						],
						"m" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "a_1_m_hashed",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"a" : 1,
						"m" : "hashed"
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

