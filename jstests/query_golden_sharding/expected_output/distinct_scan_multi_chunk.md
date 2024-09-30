TODO SERVER-92458: Shard filter out the orphan documents
### Distinct on "shardKey", with filter: { }
### Expected results
`[ "chunk1_s0_0", "chunk1_s0_1", "chunk1_s0_2", "chunk1_s1_0", "chunk1_s1_1", "chunk1_s1_2", "chunk2_s0_0", "chunk2_s0_1", "chunk2_s0_2", "chunk2_s1_0", "chunk2_s1_1", "chunk2_s1_2", "chunk3_s0_0", "chunk3_s0_1", "chunk3_s0_2", "chunk3_s1_0", "chunk3_s1_1", "chunk3_s1_2" ]`
### Distinct results
`[ "chunk1_s0_0", "chunk1_s0_0_orphan", "chunk1_s0_1", "chunk1_s0_1_orphan", "chunk1_s0_2", "chunk1_s0_2_orphan", "chunk1_s1_0", "chunk1_s1_0_orphan", "chunk1_s1_1", "chunk1_s1_1_orphan", "chunk1_s1_2", "chunk1_s1_2_orphan", "chunk2_s0_0", "chunk2_s0_0_orphan", "chunk2_s0_1", "chunk2_s0_1_orphan", "chunk2_s0_2", "chunk2_s0_2_orphan", "chunk2_s1_0", "chunk2_s1_0_orphan", "chunk2_s1_1", "chunk2_s1_1_orphan", "chunk2_s1_2", "chunk2_s1_2_orphan", "chunk3_s0_0", "chunk3_s0_0_orphan", "chunk3_s0_1", "chunk3_s0_1_orphan", "chunk3_s0_2", "chunk3_s0_2_orphan", "chunk3_s1_0", "chunk3_s1_0_orphan", "chunk3_s1_1", "chunk3_s1_1_orphan", "chunk3_s1_2", "chunk3_s1_2_orphan" ]`
### Summarized explain
```json
{
	"mergerPart" : [
		{
			"stage" : "SHARD_MERGE"
		}
	],
	"shardsPart" : {
		"distinct_scan_multi_chunk-rs0" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"shardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"shardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "shardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"shardKey" : 1
					},
					"multiKeyPaths" : {
						"shardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		},
		"distinct_scan_multi_chunk-rs1" : {
			"rejectedPlans" : [ ],
			"winningPlan" : [
				{
					"stage" : "PROJECTION_COVERED",
					"transformBy" : {
						"_id" : 0,
						"shardKey" : 1
					}
				},
				{
					"direction" : "forward",
					"indexBounds" : {
						"shardKey" : [
							"[MinKey, MaxKey]"
						]
					},
					"indexName" : "shardKey_1",
					"isFetching" : false,
					"isMultiKey" : false,
					"isPartial" : false,
					"isShardFiltering" : false,
					"isSparse" : false,
					"isUnique" : false,
					"keyPattern" : {
						"shardKey" : 1
					},
					"multiKeyPaths" : {
						"shardKey" : [ ]
					},
					"stage" : "DISTINCT_SCAN"
				}
			]
		}
	}
}
```

