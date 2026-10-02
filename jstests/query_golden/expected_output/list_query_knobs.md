## 1. Pipeline
```json
[
	{
		"$listQueryKnobs" : { }
	},
	{
		"$match" : {
			"pqsSettable" : true
		}
	},
	{
		"$unset" : [
			"name",
			"default",
			"pqsSettable"
		]
	},
	{
		"$sort" : {
			"wireName" : 1
		}
	}
]
```

PQS-settable query knobs need to be backwards compatible: wire names, types, and
validator ranges. When doing old -> new binary rollouts, persisted query settings should
continue working. For adding new knobs and/or relaxing existing ones, authors should
consider migration work to ensure that all persisted knobs remain valid on downgrades.

The C++ server parameter name, the default value, and pqsSettable are projected out:
they are not part of the PQS wire contract and can change without breaking compatibility.

## 2. Output
Number of PQS-settable knobs: 72

### cbrCEMode
```json
{
	"allowedValues" : [
		"exactCE",
		"histogramCE",
		"samplingCE",
		"heuristicCE"
	],
	"type" : "enum",
	"wireName" : "cbrCEMode"
}
```

### changeStreamRespectsReadPreference
```json
{
	"type" : "bool",
	"wireName" : "changeStreamRespectsReadPreference"
}
```

### changeStreamUpdateLookupMaxBatchSize
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : 1024
		},
		{
			"kind" : "gte",
			"value" : 1
		}
	],
	"type" : "int",
	"wireName" : "changeStreamUpdateLookupMaxBatchSize"
}
```

### changeStreamUpdateLookupMaxInputBytes
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : NumberLong(16777216)
		},
		{
			"kind" : "gte",
			"value" : NumberLong(1)
		}
	],
	"type" : "long long",
	"wireName" : "changeStreamUpdateLookupMaxInputBytes"
}
```

### changeStreamUpdateLookupMaxOutputBytes
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : NumberLong(16777216)
		},
		{
			"kind" : "gte",
			"value" : NumberLong(1)
		}
	],
	"type" : "long long",
	"wireName" : "changeStreamUpdateLookupMaxOutputBytes"
}
```

### collectionMaxDataSizeBytesToChooseHashJoin
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "collectionMaxDataSizeBytesToChooseHashJoin"
}
```

### collectionMaxNoOfDocumentsToChooseHashJoin
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "collectionMaxNoOfDocumentsToChooseHashJoin"
}
```

### collectionMaxStorageSizeBytesToChooseHashJoin
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "collectionMaxStorageSizeBytesToChooseHashJoin"
}
```

### disableLookupExecutionUsingHashJoin
```json
{
	"type" : "bool",
	"wireName" : "disableLookupExecutionUsingHashJoin"
}
```

### disablePlanCache
```json
{
	"type" : "bool",
	"wireName" : "disablePlanCache"
}
```

### disableSingleFieldExpressExecutor
```json
{
	"type" : "bool",
	"wireName" : "disableSingleFieldExpressExecutor"
}
```

### documentSourceBucketAutoMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "documentSourceBucketAutoMaxMemoryBytes"
}
```

### documentSourceDensifyMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "documentSourceDensifyMaxMemoryBytes"
}
```

### documentSourceGraphLookupMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "documentSourceGraphLookupMaxMemoryBytes"
}
```

### documentSourceGroupMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "documentSourceGroupMaxMemoryBytes"
}
```

### documentSourceLookupCacheSizeBytes
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "documentSourceLookupCacheSizeBytes"
}
```

### documentSourceSetWindowFieldsMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "documentSourceSetWindowFieldsMaxMemoryBytes"
}
```

### enableChangeStreamMatchExpressionReordering
```json
{
	"type" : "bool",
	"wireName" : "enableChangeStreamMatchExpressionReordering"
}
```

### enumerationMaxIntersectPerAnd
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "enumerationMaxIntersectPerAnd"
}
```

### enumerationMaxOrSolutions
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "enumerationMaxOrSolutions"
}
```

### enumerationPreferLockstepOrEnumeration
```json
{
	"type" : "bool",
	"wireName" : "enumerationPreferLockstepOrEnumeration"
}
```

### forceIntersectionPlans
```json
{
	"type" : "bool",
	"wireName" : "forceIntersectionPlans"
}
```

### maxEstimatedScanBytes
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : NumberLong(-1)
		}
	],
	"type" : "long long",
	"wireName" : "maxEstimatedScanBytes"
}
```

### maxEstimatedScanBytesDryRun
```json
{
	"type" : "bool",
	"wireName" : "maxEstimatedScanBytesDryRun"
}
```

### maxScansToExplode
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "maxScansToExplode"
}
```

### minRequiredImprovementRatioForCostBasedRankerChoice
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : 10
		},
		{
			"kind" : "gte",
			"value" : 1
		}
	],
	"type" : "double",
	"wireName" : "minRequiredImprovementRatioForCostBasedRankerChoice"
}
```

### mixedPlanRankingStrategy
```json
{
	"allowedValues" : [
		"NoMultiplanningResults",
		"EstimateRankingEffort"
	],
	"type" : "enum",
	"wireName" : "mixedPlanRankingStrategy"
}
```

### nearStageMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "nearStageMaxMemoryBytes"
}
```

### noTableScan
```json
{
	"type" : "bool",
	"wireName" : "noTableScan"
}
```

### numWorksPerPlanForMPEstimation
```json
{
	"bounds" : [
		{
			"kind" : "lt",
			"value" : 10000
		},
		{
			"kind" : "gt",
			"value" : 100
		}
	],
	"type" : "int",
	"wireName" : "numWorksPerPlanForMPEstimation"
}
```

### operationResponseMaxMS
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "operationResponseMaxMS"
}
```

### planEvaluationCollFraction
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : 1
		},
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "double",
	"wireName" : "planEvaluationCollFraction"
}
```

### planEvaluationMaxResults
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "planEvaluationMaxResults"
}
```

### planEvaluationWorks
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "planEvaluationWorks"
}
```

### planOrChildrenIndependently
```json
{
	"type" : "bool",
	"wireName" : "planOrChildrenIndependently"
}
```

### planRanker
```json
{
	"allowedValues" : [
		"multiPlanning",
		"costBased",
		"mixed"
	],
	"type" : "enum",
	"wireName" : "planRanker"
}
```

### planTieBreakingWithIndexHeuristics
```json
{
	"type" : "bool",
	"wireName" : "planTieBreakingWithIndexHeuristics"
}
```

### planTotalEvaluationCollFraction
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "double",
	"wireName" : "planTotalEvaluationCollFraction"
}
```

### plannerEnableCountScanForUnfilteredCount
```json
{
	"type" : "bool",
	"wireName" : "plannerEnableCountScanForUnfilteredCount"
}
```

### plannerEnableHashIntersection
```json
{
	"type" : "bool",
	"wireName" : "plannerEnableHashIntersection"
}
```

### plannerEnableIndexIntersection
```json
{
	"type" : "bool",
	"wireName" : "plannerEnableIndexIntersection"
}
```

### plannerEnableIndexPruning
```json
{
	"type" : "bool",
	"wireName" : "plannerEnableIndexPruning"
}
```

### plannerEnableSortIndexIntersection
```json
{
	"type" : "bool",
	"wireName" : "plannerEnableSortIndexIntersection"
}
```

### plannerGenerateCoveredWholeIndexScans
```json
{
	"type" : "bool",
	"wireName" : "plannerGenerateCoveredWholeIndexScans"
}
```

### plannerMaxIndexedSolutions
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "plannerMaxIndexedSolutions"
}
```

### plannerPushdownFilterToIxscanForSort
```json
{
	"type" : "bool",
	"wireName" : "plannerPushdownFilterToIxscanForSort"
}
```

### plannerUseMultiplannerForSingleSolutions
```json
{
	"type" : "bool",
	"wireName" : "plannerUseMultiplannerForSingleSolutions"
}
```

### queryMaxAddToSetBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "queryMaxAddToSetBytes"
}
```

### queryMaxConcatArraysBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "queryMaxConcatArraysBytes"
}
```

### queryMaxMemoryUsageBytesPerOperation
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "queryMaxMemoryUsageBytesPerOperation"
}
```

### queryMaxPercentileAccumulatorBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "queryMaxPercentileAccumulatorBytes"
}
```

### queryMaxPushBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		},
		{
			"kind" : "lte",
			"value" : NumberLong(2147483647)
		}
	],
	"type" : "long long",
	"wireName" : "queryMaxPushBytes"
}
```

### queryMaxSetUnionBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "queryMaxSetUnionBytes"
}
```

### queryMaxSpoolMemoryUsageBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "queryMaxSpoolMemoryUsageBytes"
}
```

### queryTopNAccumulatorBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		},
		{
			"kind" : "lte",
			"value" : NumberLong(2147483647)
		}
	],
	"type" : "long long",
	"wireName" : "queryTopNAccumulatorBytes"
}
```

### samplingCEMethod
```json
{
	"allowedValues" : [
		"random",
		"chunk"
	],
	"type" : "enum",
	"wireName" : "samplingCEMethod"
}
```

### samplingCEMethodForPersistentSamples
```json
{
	"allowedValues" : [
		"random",
		"chunk"
	],
	"type" : "enum",
	"wireName" : "samplingCEMethodForPersistentSamples"
}
```

### samplingConfidenceInterval
```json
{
	"allowedValues" : [
		"90",
		"95",
		"99"
	],
	"type" : "enum",
	"wireName" : "samplingConfidenceInterval"
}
```

### samplingMarginOfError
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : 10
		},
		{
			"kind" : "gte",
			"value" : 1
		}
	],
	"type" : "double",
	"wireName" : "samplingMarginOfError"
}
```

### samplingSizeOverride
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "samplingSizeOverride"
}
```

### sbeDisableGroupPushdown
```json
{
	"type" : "bool",
	"wireName" : "sbeDisableGroupPushdown"
}
```

### sbeDisableLookupPushdown
```json
{
	"type" : "bool",
	"wireName" : "sbeDisableLookupPushdown"
}
```

### sbeDisableLookupUnwindPushdown
```json
{
	"type" : "bool",
	"wireName" : "sbeDisableLookupUnwindPushdown"
}
```

### sbeDisableTimeSeriesPushdown
```json
{
	"type" : "bool",
	"wireName" : "sbeDisableTimeSeriesPushdown"
}
```

### sbeHashAggApproxMemoryUseInBytesBeforeSpill
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "sbeHashAggApproxMemoryUseInBytesBeforeSpill"
}
```

### sbeHashJoinApproxMemoryUseInBytesBeforeSpill
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "sbeHashJoinApproxMemoryUseInBytesBeforeSpill"
}
```

### sbeHashLookupApproxMemoryUseInBytesBeforeSpill
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "sbeHashLookupApproxMemoryUseInBytesBeforeSpill"
}
```

### sbePlanEvaluationMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gte",
			"value" : 0
		}
	],
	"type" : "int",
	"wireName" : "sbePlanEvaluationMaxMemoryBytes"
}
```

### searchIdLookupMaxBatchSize
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : 1024
		},
		{
			"kind" : "gte",
			"value" : 1
		}
	],
	"type" : "int",
	"wireName" : "searchIdLookupMaxBatchSize"
}
```

### searchIdLookupMaxInputBytes
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : NumberLong(16777216)
		},
		{
			"kind" : "gte",
			"value" : NumberLong(1)
		}
	],
	"type" : "long long",
	"wireName" : "searchIdLookupMaxInputBytes"
}
```

### searchIdLookupMaxOutputBytes
```json
{
	"bounds" : [
		{
			"kind" : "lte",
			"value" : NumberLong(16777216)
		},
		{
			"kind" : "gte",
			"value" : NumberLong(1)
		}
	],
	"type" : "long long",
	"wireName" : "searchIdLookupMaxOutputBytes"
}
```

### textOrStageMaxMemoryBytes
```json
{
	"bounds" : [
		{
			"kind" : "gt",
			"value" : NumberLong(0)
		}
	],
	"type" : "long long",
	"wireName" : "textOrStageMaxMemoryBytes"
}
```

