/**
 * Tests to validate the input values accepted by internal query server parameters. The test
 * verfies that the system responds with the expected error code for input values that fall outside
 * each parameter's valid bounds, and correctly applies input values which fall within that
 * parameter's valid bounds.
 */

(function() {
"use strict";

const conn = MongoRunner.runMongod();
const testDB = conn.getDB("admin");
const expectedParamDefaults = {
    internalQueryPlanEvaluationWorks: 10000,
    internalQueryPlanEvaluationCollFraction: 0.3,
    internalQueryPlanEvaluationMaxResults: 101,
    internalQueryCacheSize: 5000,
    internalQueryCacheFeedbacksStored: 20,
    internalQueryCacheEvictionRatio: 10.0,
    internalQueryCacheWorksGrowthCoefficient: 2.0,
    internalQueryCacheDisableInactiveEntries: false,
    internalQueryCacheListPlansNewOutput: false,
    internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: 512 * 1024 * 1024,
    internalQueryPlannerMaxIndexedSolutions: 64,
    internalQueryEnumerationMaxOrSolutions: 10,
    internalQueryEnumerationMaxIntersectPerAnd: 3,
    internalQueryForceIntersectionPlans: false,
    internalQueryPlannerEnableIndexIntersection: true,
    internalQueryPlannerEnableHashIntersection: false,
    internalQueryPlanOrChildrenIndependently: true,
    internalQueryMaxScansToExplode: 200,
    internalQueryExecMaxBlockingSortBytes: 32 * 1024 * 1024,
    internalQueryExecYieldIterations: 128,
    internalQueryExecYieldPeriodMS: 10,
    internalQueryFacetBufferSizeBytes: 100 * 1024 * 1024,
    internalDocumentSourceCursorBatchSizeBytes: 4 * 1024 * 1024,
    internalDocumentSourceLookupCacheSizeBytes: 100 * 1024 * 1024,
    internalDocumentSourceSortMaxBlockingSortBytes: 100 * 1024 * 1024,
    internalLookupStageIntermediateDocumentMaxSizeBytes: 100 * 1024 * 1024,
    internalDocumentSourceGroupMaxMemoryBytes: 100 * 1024 * 1024,
    internalPipelineLengthLimit: 2147483647,  // INT_MAX
    internalQueryMaxPushBytes: 100 * 1024 * 1024,
    internalQueryMaxRangeBytes: 100 * 1024 * 1024,
    internalQueryMaxAddToSetBytes: 100 * 1024 * 1024,
    // Should be half the value of 'internalQueryExecYieldIterations' parameter.
    internalInsertMaxBatchSize: 64,
    internalQueryPlannerGenerateCoveredWholeIndexScans: false,
    internalQueryIgnoreUnknownJSONSchemaKeywords: false,
    internalQueryProhibitBlockingMergeOnMongoS: false,
};

function assertDefaultParameterValues() {
    // For each parameter in 'expectedParamDefaults' verify that the value returned by
    // 'getParameter' is same as the expected value.
    for (let paramName in expectedParamDefaults) {
        const expectedParamValue = expectedParamDefaults[paramName];
        const getParamRes =
            assert.commandWorked(testDB.adminCommand({getParameter: 1, [paramName]: 1}));
        assert.eq(getParamRes[paramName], expectedParamValue);
    }
}

function assertSetParameterSucceeds(paramName, value) {
    assert.commandWorked(testDB.adminCommand({setParameter: 1, [paramName]: value}));
    // Verify that the set parameter actually worked by doing a get and verifying the value.
    const getParamRes =
        assert.commandWorked(testDB.adminCommand({getParameter: 1, [paramName]: 1}));
    assert.eq(getParamRes[paramName], value);
}

function assertSetParameterFails(paramName, value) {
    assert.commandFailedWithCode(testDB.adminCommand({setParameter: 1, [paramName]: value}),
                                 ErrorCodes.BadValue);
}

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

assertSetParameterSucceeds("internalQueryCacheSize", 1);
assertSetParameterSucceeds("internalQueryCacheSize", 0);
assertSetParameterFails("internalQueryCacheSize", -1);

assertSetParameterSucceeds("internalQueryCacheFeedbacksStored", 1);
assertSetParameterSucceeds("internalQueryCacheFeedbacksStored", 0);
assertSetParameterFails("internalQueryCacheFeedbacksStored", -1);

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

assertSetParameterSucceeds("internalQueryExecMaxBlockingSortBytes", 11);
assertSetParameterSucceeds("internalQueryExecMaxBlockingSortBytes", 0);
assertSetParameterFails("internalQueryExecMaxBlockingSortBytes", -1);

assertSetParameterSucceeds("internalQueryExecYieldIterations", 10);
assertSetParameterSucceeds("internalQueryExecYieldIterations", 0);
assertSetParameterSucceeds("internalQueryExecYieldIterations", -1);

assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 1);
assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 0);
assertSetParameterFails("internalQueryExecYieldPeriodMS", -1);

assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 11);
assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 0);
assertSetParameterFails("internalQueryExecYieldPeriodMS", -1);

assertSetParameterSucceeds("internalQueryFacetBufferSizeBytes", 1);
assertSetParameterFails("internalQueryFacetBufferSizeBytes", 0);
assertSetParameterFails("internalQueryFacetBufferSizeBytes", -1);

assertSetParameterSucceeds("internalDocumentSourceSortMaxBlockingSortBytes", 11);
assertSetParameterFails("internalDocumentSourceSortMaxBlockingSortBytes", 0);
assertSetParameterFails("internalDocumentSourceSortMaxBlockingSortBytes", -1);

assertSetParameterSucceeds("internalDocumentSourceGroupMaxMemoryBytes", 11);
assertSetParameterFails("internalDocumentSourceGroupMaxMemoryBytes", 0);
assertSetParameterFails("internalDocumentSourceGroupMaxMemoryBytes", -1);

assertSetParameterSucceeds("internalQueryMaxPushBytes", 10);
assertSetParameterFails("internalQueryMaxPushBytes", 0);
assertSetParameterFails("internalQueryMaxPushBytes", -1);

assertSetParameterSucceeds("internalQueryMaxAddToSetBytes", 10);
assertSetParameterFails("internalQueryMaxAddToSetBytes", 0);
assertSetParameterFails("internalQueryMaxAddToSetBytes", -1);

// Internal BSON max object size is slightly larger than the max user object size, to
// accommodate command metadata.
const bsonUserSizeLimit = assert.commandWorked(testDB.isMaster()).maxBsonObjectSize;
const bsonObjMaxInternalSize = bsonUserSizeLimit + 16 * 1024;

assertSetParameterFails("internalLookupStageIntermediateDocumentMaxSizeBytes", 1);
assertSetParameterSucceeds("internalLookupStageIntermediateDocumentMaxSizeBytes",
                           bsonObjMaxInternalSize);

assertSetParameterSucceeds("internalInsertMaxBatchSize", 11);
assertSetParameterFails("internalInsertMaxBatchSize", 0);
assertSetParameterFails("internalInsertMaxBatchSize", -1);

assertSetParameterSucceeds("internalDocumentSourceCursorBatchSizeBytes", 11);
assertSetParameterSucceeds("internalDocumentSourceCursorBatchSizeBytes", 0);
assertSetParameterFails("internalDocumentSourceCursorBatchSizeBytes", -1);

assertSetParameterSucceeds("internalDocumentSourceLookupCacheSizeBytes", 11);
assertSetParameterSucceeds("internalDocumentSourceLookupCacheSizeBytes", 0);
assertSetParameterFails("internalDocumentSourceLookupCacheSizeBytes", -1);

MongoRunner.stopMongod(conn);
})();
