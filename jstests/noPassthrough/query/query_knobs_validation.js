/**
 * Tests to validate the input values accepted by internal query server parameters. The test
 * verifies that the system responds with the expected error code for input values that fall outside
 * each parameter's valid bounds, and correctly applies input values which fall within that
 * parameter's valid bounds.
 */

import {getExpectedPipelineLimit} from "jstests/libs/query/aggregation_pipeline_utils.js";

const conn = MongoRunner.runMongod();
const testDB = conn.getDB("admin");
const expectedParamDefaults = {
    internalQueryPlanEvaluationWorks: 10000,
    internalQueryPlanEvaluationCollFraction: 0.3,
    internalQueryPlanEvaluationMaxResults: 101,
    internalQueryCacheMaxEntriesPerCollection: 5000,
    // This is a deprecated alias for "internalQueryCacheMaxEntriesPerCollection".
    internalQueryCacheSize: 5000,
    internalQueryCacheEvictionRatio: 10.0,
    internalQueryCacheWorksGrowthCoefficient: 2.0,
    internalQueryCacheDisableInactiveEntries: false,
    internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: 512 * 1024 * 1024,
    internalQueryPlannerMaxIndexedSolutions: 64,
    internalQueryEnumerationMaxOrSolutions: 10,
    internalQueryEnumerationMaxIntersectPerAnd: 3,
    internalQueryForceIntersectionPlans: false,
    internalQueryPlannerEnableIndexIntersection: true,
    internalQueryPlannerEnableHashIntersection: false,
    internalQueryPlannerEnableSortIndexIntersection: false,
    internalQueryPlanOrChildrenIndependently: true,
    internalQueryMaxScansToExplode: 200,
    internalQueryMaxBlockingSortMemoryUsageBytes: 100 * 1024 * 1024,
    internalQueryExecYieldIterations: -1,
    internalQueryExecYieldPeriodMS: 10,
    internalQueryExpressionInterruptIterations: 10,
    internalQueryExpressionInterruptPeriodMS: 1000,
    internalQueryFacetBufferSizeBytes: 100 * 1024 * 1024,
    internalDocumentSourceCursorBatchSizeBytes: 4 * 1024 * 1024,
    internalDocumentSourceCursorInitialBatchSize: 32,
    internalDocumentSourceLookupCacheSizeBytes: 100 * 1024 * 1024,
    internalLookupStageIntermediateDocumentMaxSizeBytes: 100 * 1024 * 1024,
    internalDocumentSourceGroupMaxMemoryBytes: 100 * 1024 * 1024,
    internalDocumentSourceSetWindowFieldsMaxMemoryBytes: 100 * 1024 * 1024,
    internalQueryMaxJsEmitBytes: 100 * 1024 * 1024,
    internalQueryMaxPushBytes: 100 * 1024 * 1024,
    internalQueryMaxRangeBytes: 100 * 1024 * 1024,
    internalQueryMaxMapReduceExpressionBytes: 100 * 1024 * 1024,
    internalQueryMaxAddToSetBytes: 100 * 1024 * 1024,
    internalQueryMaxConcatArraysBytes: 100 * 1024 * 1024,
    internalQueryMaxSetUnionBytes: 100 * 1024 * 1024,
    internalQueryPlannerGenerateCoveredWholeIndexScans: false,
    internalQueryIgnoreUnknownJSONSchemaKeywords: false,
    internalQueryProhibitBlockingMergeOnMongoS: false,
    internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals: 1000,
    internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: 10 * 1000,
    internalQueryCollectionMaxDataSizeBytesToChooseHashJoin: 100 * 1024 * 1024,
    internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin: 100 * 1024 * 1024,
    internalQueryFLERewriteMemoryLimit: 14 * 1024 * 1024,
    internalQueryDisableLookupExecutionUsingHashJoin: false,
    internalQuerySlotBasedExecutionDisableLookupPushdown: false,
    internalQuerySlotBasedExecutionDisableGroupPushdown: false,
    allowDiskUseByDefault: true,
    internalQueryMaxSpoolMemoryUsageBytes: 100 * 1024 * 1024,
    internalQueryMaxSpoolDiskUsageBytes: 10 * 100 * 1024 * 1024,
    internalQueryDocumentSourceWriterBatchExtraReservedBytes: 0,
    internalQuerySlotBasedExecutionDisableTimeSeriesPushdown: false,
    internalQueryCollectOptimizerMetrics: false,
    internalQueryDisablePlanCache: false,
    internalQueryFindCommandBatchSize: 101,
    internalQuerySlotBasedExecutionHashAggIncreasedSpilling: "inDebug",
    internalQueryUnifiedWriteExecutor: false,
    internalOrStageMaxMemoryBytes: 100 * 1024 * 1024,
};

function assertDefaultParameterValues() {
    // For each parameter in 'expectedParamDefaults' verify that the value returned by
    // 'getParameter' is same as the expected value.
    for (let paramName in expectedParamDefaults) {
        const expectedParamValue = expectedParamDefaults[paramName];
        const getParamRes = assert.commandWorked(testDB.adminCommand({getParameter: 1, [paramName]: 1}));
        assert.eq(getParamRes[paramName], expectedParamValue);
    }
}

function assertSetParameterSucceeds(paramName, value) {
    assert.commandWorked(testDB.adminCommand({setParameter: 1, [paramName]: value}));
    // Verify that the set parameter actually worked by doing a get and verifying the value.
    const getParamRes = assert.commandWorked(testDB.adminCommand({getParameter: 1, [paramName]: 1}));
    assert.eq(getParamRes[paramName], value);
}

function assertSetParameterFails(paramName, value) {
    assert.commandFailedWithCode(testDB.adminCommand({setParameter: 1, [paramName]: value}), ErrorCodes.BadValue);
}

const getParamRes = assert.commandWorked(testDB.adminCommand({getParameter: 1, internalPipelineLengthLimit: 1}));
assert.eq(getParamRes["internalPipelineLengthLimit"], getExpectedPipelineLimit(testDB));

// Verify that the default values are set as expected when the server starts up.
assertDefaultParameterValues();

assertSetParameterSucceeds("internalQueryPlanEvaluationWorks", 11);
assertSetParameterFails("internalQueryPlanEvaluationWorks", 0);
assertSetParameterFails("internalQueryPlanEvaluationWorks", -1);

assertSetParameterSucceeds("internalQueryPlanEvaluationCollFraction", 0.0);
assertSetParameterSucceeds("internalQueryPlanEvaluationCollFraction", 0.444);
assertSetParameterSucceeds("internalQueryPlanEvaluationCollFraction", 1.0);
assertSetParameterFails("internalQueryPlanEvaluationCollFraction", -0.1);
assertSetParameterFails("internalQueryPlanEvaluationCollFraction", 1.0001);

assertSetParameterSucceeds("internalQueryPlanEvaluationMaxResults", 11);
assertSetParameterSucceeds("internalQueryPlanEvaluationMaxResults", 0);
assertSetParameterFails("internalQueryPlanEvaluationMaxResults", -1);

assertSetParameterSucceeds("internalQueryCacheMaxEntriesPerCollection", 1);
assertSetParameterSucceeds("internalQueryCacheMaxEntriesPerCollection", 0);
assertSetParameterFails("internalQueryCacheMaxEntriesPerCollection", -1);
// "internalQueryCacheSize" is a deprecated alias for "internalQueryCacheMaxEntriesPerCollection".
assertSetParameterSucceeds("internalQueryCacheSize", 1);
assertSetParameterSucceeds("internalQueryCacheSize", 0);
assertSetParameterFails("internalQueryCacheSize", -1);

assertSetParameterSucceeds("internalQueryCacheMaxSizeBytesBeforeStripDebugInfo", 1);
assertSetParameterSucceeds("internalQueryCacheMaxSizeBytesBeforeStripDebugInfo", 0);
assertSetParameterFails("internalQueryCacheMaxSizeBytesBeforeStripDebugInfo", -1);

assertSetParameterSucceeds("internalQueryCacheEvictionRatio", 1.0);
assertSetParameterSucceeds("internalQueryCacheEvictionRatio", 0.0);
assertSetParameterFails("internalQueryCacheEvictionRatio", -0.1);

assertSetParameterSucceeds("internalQueryCacheWorksGrowthCoefficient", 1.1);
assertSetParameterFails("internalQueryCacheWorksGrowthCoefficient", 1.0);
assertSetParameterFails("internalQueryCacheWorksGrowthCoefficient", 0.1);

assertSetParameterSucceeds("internalQueryPlannerMaxIndexedSolutions", 11);
assertSetParameterSucceeds("internalQueryPlannerMaxIndexedSolutions", 0);
assertSetParameterFails("internalQueryPlannerMaxIndexedSolutions", -1);

assertSetParameterSucceeds("internalQueryEnumerationMaxOrSolutions", 11);
assertSetParameterSucceeds("internalQueryEnumerationMaxOrSolutions", 0);
assertSetParameterFails("internalQueryEnumerationMaxOrSolutions", -1);

assertSetParameterSucceeds("internalQueryEnumerationMaxIntersectPerAnd", 11);
assertSetParameterSucceeds("internalQueryEnumerationMaxIntersectPerAnd", 0);
assertSetParameterFails("internalQueryEnumerationMaxIntersectPerAnd", -1);

assertSetParameterSucceeds("internalQueryMaxScansToExplode", 11);
assertSetParameterSucceeds("internalQueryMaxScansToExplode", 0);
assertSetParameterFails("internalQueryMaxScansToExplode", -1);

assertSetParameterSucceeds("internalQueryMaxBlockingSortMemoryUsageBytes", 11);
assertSetParameterSucceeds("internalQueryMaxBlockingSortMemoryUsageBytes", 0);
assertSetParameterFails("internalQueryMaxBlockingSortMemoryUsageBytes", -1);

assertSetParameterSucceeds("internalQueryExecYieldIterations", 10);
assertSetParameterSucceeds("internalQueryExecYieldIterations", 0);
assertSetParameterSucceeds("internalQueryExecYieldIterations", -1);

assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 1);
assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 0);
assertSetParameterFails("internalQueryExecYieldPeriodMS", -1);

assertSetParameterSucceeds("internalQueryExpressionInterruptIterations", 10);
assertSetParameterSucceeds("internalQueryExpressionInterruptIterations", 0);
assertSetParameterSucceeds("internalQueryExpressionInterruptIterations", -1);

assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 11);
assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 0);
assertSetParameterFails("internalQueryExecYieldPeriodMS", -1);

assertSetParameterSucceeds("internalQueryExpressionInterruptPeriodMS", 11);
assertSetParameterSucceeds("internalQueryExpressionInterruptPeriodMS", 0);
assertSetParameterFails("internalQueryExpressionInterruptPeriodMS", -1);

assertSetParameterSucceeds("internalQueryFacetBufferSizeBytes", 1);
assertSetParameterFails("internalQueryFacetBufferSizeBytes", 0);
assertSetParameterFails("internalQueryFacetBufferSizeBytes", -1);

assertSetParameterSucceeds("internalDocumentSourceGroupMaxMemoryBytes", 11);
assertSetParameterFails("internalDocumentSourceGroupMaxMemoryBytes", 0);
assertSetParameterFails("internalDocumentSourceGroupMaxMemoryBytes", -1);

assertSetParameterSucceeds("internalDocumentSourceSetWindowFieldsMaxMemoryBytes", 11);
assertSetParameterFails("internalDocumentSourceSetWindowFieldsMaxMemoryBytes", 0);
assertSetParameterFails("internalDocumentSourceSetWindowFieldsMaxMemoryBytes", -1);

assertSetParameterSucceeds("internalQueryMaxJsEmitBytes", 10);
assertSetParameterFails("internalQueryMaxJsEmitBytes", 0);
assertSetParameterFails("internalQueryMaxJsEmitBytes", -1);

assertSetParameterSucceeds("internalQueryMaxPushBytes", 10);
assertSetParameterFails("internalQueryMaxPushBytes", 0);
assertSetParameterFails("internalQueryMaxPushBytes", -1);

assertSetParameterSucceeds("internalQueryMaxRangeBytes", 10);
assertSetParameterFails("internalQueryMaxRangeBytes", 0);
assertSetParameterFails("internalQueryMaxRangeBytes", -1);

assertSetParameterSucceeds("internalQueryMaxMapReduceExpressionBytes", 10);
assertSetParameterFails("internalQueryMaxMapReduceExpressionBytes", 0);
assertSetParameterFails("internalQueryMaxMapReduceExpressionBytes", -1);

assertSetParameterSucceeds("internalQueryMaxAddToSetBytes", 10);
assertSetParameterFails("internalQueryMaxAddToSetBytes", 0);
assertSetParameterFails("internalQueryMaxAddToSetBytes", -1);

assertSetParameterSucceeds("internalQueryMaxConcatArraysBytes", 10);
assertSetParameterFails("internalQueryMaxConcatArraysBytes", 0);
assertSetParameterFails("internalQueryMaxConcatArraysBytes", -1);

assertSetParameterSucceeds("internalQueryMaxSetUnionBytes", 10);
assertSetParameterFails("internalQueryMaxSetUnionBytes", 0);
assertSetParameterFails("internalQueryMaxSetUnionBytes", -1);

// Internal BSON max object size is slightly larger than the max user object size, to
// accommodate command metadata.
const bsonUserSizeLimit = assert.commandWorked(testDB.hello()).maxBsonObjectSize;
const bsonObjMaxInternalSize = bsonUserSizeLimit + 16 * 1024;

assertSetParameterFails("internalLookupStageIntermediateDocumentMaxSizeBytes", 1);
assertSetParameterSucceeds("internalLookupStageIntermediateDocumentMaxSizeBytes", bsonObjMaxInternalSize);

assertSetParameterSucceeds("internalInsertMaxBatchSize", 11);
assertSetParameterFails("internalInsertMaxBatchSize", 0);
assertSetParameterFails("internalInsertMaxBatchSize", -1);

assertSetParameterSucceeds("internalDocumentSourceCursorBatchSizeBytes", 11);
assertSetParameterSucceeds("internalDocumentSourceCursorBatchSizeBytes", 0);
assertSetParameterFails("internalDocumentSourceCursorBatchSizeBytes", -1);

assertSetParameterSucceeds("internalDocumentSourceCursorInitialBatchSize", 11);
assertSetParameterSucceeds("internalDocumentSourceCursorInitialBatchSize", 0);
assertSetParameterFails("internalDocumentSourceCursorInitialBatchSize", -1);

assertSetParameterSucceeds("internalDocumentSourceLookupCacheSizeBytes", 11);
assertSetParameterSucceeds("internalDocumentSourceLookupCacheSizeBytes", 0);
assertSetParameterFails("internalDocumentSourceLookupCacheSizeBytes", -1);

assertSetParameterSucceeds("internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals", 1001);
assertSetParameterFails("internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals", 0);
assertSetParameterFails("internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals", -1);

assertSetParameterSucceeds("internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin", 1);
assertSetParameterFails("internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin", 0);
assertSetParameterFails("internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin", -1);

assertSetParameterSucceeds("internalQueryCollectionMaxDataSizeBytesToChooseHashJoin", 100);
assertSetParameterFails("internalQueryCollectionMaxDataSizeBytesToChooseHashJoin", 0);
assertSetParameterFails("internalQueryCollectionMaxDataSizeBytesToChooseHashJoin", -1);

assertSetParameterSucceeds("internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin", 100);
assertSetParameterFails("internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin", 0);
assertSetParameterFails("internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin", -1);

assertSetParameterSucceeds("internalQueryDisableLookupExecutionUsingHashJoin", true);
assertSetParameterSucceeds("internalQueryDisableLookupExecutionUsingHashJoin", false);

assertSetParameterSucceeds("internalQuerySlotBasedExecutionDisableLookupPushdown", true);
assertSetParameterSucceeds("internalQuerySlotBasedExecutionDisableLookupPushdown", false);

assertSetParameterSucceeds("internalQuerySlotBasedExecutionDisableGroupPushdown", true);
assertSetParameterSucceeds("internalQuerySlotBasedExecutionDisableGroupPushdown", false);

assertSetParameterSucceeds("allowDiskUseByDefault", false);
assertSetParameterSucceeds("allowDiskUseByDefault", true);

assertSetParameterSucceeds("internalQueryFLERewriteMemoryLimit", 14 * 1024 * 1024);
assertSetParameterFails("internalQueryFLERewriteMemoryLimit", 0);

assertSetParameterSucceeds("internalQueryFrameworkControl", "forceClassicEngine");
assertSetParameterSucceeds("internalQueryFrameworkControl", "trySbeEngine");
assertSetParameterFails("internalQueryFrameworkControl", "tryCascades");
assertSetParameterFails("internalQueryFrameworkControl", 1);

assertSetParameterSucceeds("internalQueryMaxSpoolMemoryUsageBytes", 100);
assertSetParameterSucceeds("internalQueryMaxSpoolMemoryUsageBytes", 1);
assertSetParameterFails("internalQueryMaxSpoolMemoryUsageBytes", 0);

assertSetParameterSucceeds("internalQueryMaxSpoolDiskUsageBytes", 100);
assertSetParameterSucceeds("internalQueryMaxSpoolDiskUsageBytes", 1);
assertSetParameterFails("internalQueryMaxSpoolDiskUsageBytes", 0);

assertSetParameterSucceeds("internalQueryDocumentSourceWriterBatchExtraReservedBytes", 10);
assertSetParameterSucceeds("internalQueryDocumentSourceWriterBatchExtraReservedBytes", 4 * 1024 * 1024);
assertSetParameterFails("internalQueryDocumentSourceWriterBatchExtraReservedBytes", -1);
assertSetParameterFails("internalQueryDocumentSourceWriterBatchExtraReservedBytes", 9 * 1024 * 1024);

assertSetParameterSucceeds("internalQuerySlotBasedExecutionDisableTimeSeriesPushdown", true);
assertSetParameterSucceeds("internalQuerySlotBasedExecutionDisableTimeSeriesPushdown", false);

assertSetParameterSucceeds("internalQueryCollectOptimizerMetrics", true);
assertSetParameterSucceeds("internalQueryCollectOptimizerMetrics", false);

assertSetParameterSucceeds("internalQueryDisablePlanCache", true);
assertSetParameterSucceeds("internalQueryDisablePlanCache", false);

assertSetParameterSucceeds("internalQueryFindCommandBatchSize", 30);
assertSetParameterFails("internalQueryFindCommandBatchSize", 0);
assertSetParameterFails("internalQueryFindCommandBatchSize", -1);

assertSetParameterSucceeds("internalTextOrStageMaxMemoryBytes", 100 * 1024 * 1024);
assertSetParameterFails("internalTextOrStageMaxMemoryBytes", 0);
assertSetParameterFails("internalTextOrStageMaxMemoryBytes", -1);

assertSetParameterSucceeds("internalQuerySlotBasedExecutionHashAggIncreasedSpilling", "never");
assertSetParameterSucceeds("internalQuerySlotBasedExecutionHashAggIncreasedSpilling", "always");
assertSetParameterSucceeds("internalQuerySlotBasedExecutionHashAggIncreasedSpilling", "inDebug");
assertSetParameterFails("internalQuerySlotBasedExecutionHashAggIncreasedSpilling", "random");

assertSetParameterSucceeds("internalQueryUnifiedWriteExecutor", true);
assertSetParameterSucceeds("internalQueryUnifiedWriteExecutor", false);

assertSetParameterSucceeds("internalOrStageMaxMemoryBytes", 11);
assertSetParameterFails("internalOrStageMaxMemoryBytes", 0);
assertSetParameterFails("internalOrStageMaxMemoryBytes", -1);

MongoRunner.stopMongod(conn);
