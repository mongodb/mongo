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
    internalQueryPlanEvaluationWorksSbe: 10000,
    internalQueryPlanEvaluationCollFraction: 0.3,
    internalQueryPlanEvaluationCollFractionSbe: 0.0,
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
    internalQueryPlanOrChildrenIndependently: true,
    internalQueryMaxScansToExplode: 200,
    internalQueryMaxBlockingSortMemoryUsageBytes: 100 * 1024 * 1024,
    internalQueryExecYieldIterations: 1000,
    internalQueryExecYieldPeriodMS: 10,
    internalQueryFacetBufferSizeBytes: 100 * 1024 * 1024,
    internalDocumentSourceCursorBatchSizeBytes: 4 * 1024 * 1024,
    internalDocumentSourceLookupCacheSizeBytes: 100 * 1024 * 1024,
    internalLookupStageIntermediateDocumentMaxSizeBytes: 100 * 1024 * 1024,
    internalDocumentSourceGroupMaxMemoryBytes: 100 * 1024 * 1024,
    internalDocumentSourceSetWindowFieldsMaxMemoryBytes: 100 * 1024 * 1024,
    internalQueryMaxJsEmitBytes: 100 * 1024 * 1024,
    internalQueryMaxPushBytes: 100 * 1024 * 1024,
    internalQueryMaxRangeBytes: 100 * 1024 * 1024,
    internalQueryMaxAddToSetBytes: 100 * 1024 * 1024,
    // Should be half the value of 'internalQueryExecYieldIterations' parameter.
    internalInsertMaxBatchSize: 500,
    internalQueryPlannerGenerateCoveredWholeIndexScans: false,
    internalQueryIgnoreUnknownJSONSchemaKeywords: false,
    internalQueryProhibitBlockingMergeOnMongoS: false,
    internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals: 1000
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

// InternalPipelineLengthLimit is different is lowered in a debug build so its expected value
// depends on that
const getParamRes =
    assert.commandWorked(testDB.adminCommand({getParameter: 1, internalPipelineLengthLimit: 1}));
assert.eq(getParamRes["internalPipelineLengthLimit"],
          testDB.adminCommand("buildInfo").debug ? 200 : 1000);

// Verify that the default values are set as expected when the server starts up.
assertDefaultParameterValues();

for (let paramName of ["internalQueryPlanEvaluationWorks", "internalQueryPlanEvaluationWorksSbe"]) {
    assertSetParameterSucceeds(paramName, 11);
    assertSetParameterFails(paramName, 0);
    assertSetParameterFails(paramName, -1);
}

for (let paramName of ["internalQueryPlanEvaluationCollFraction",
                       "internalQueryPlanEvaluationCollFractionSbe"]) {
    assertSetParameterSucceeds(paramName, 0.0);
    assertSetParameterSucceeds(paramName, 0.444);
    assertSetParameterSucceeds(paramName, 1.0);
    assertSetParameterFails(paramName, -0.1);
    assertSetParameterFails(paramName, 1.0001);
}

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

assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 11);
assertSetParameterSucceeds("internalQueryExecYieldPeriodMS", 0);
assertSetParameterFails("internalQueryExecYieldPeriodMS", -1);

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

assertSetParameterSucceeds("internalQueryMaxAddToSetBytes", 10);
assertSetParameterFails("internalQueryMaxAddToSetBytes", 0);
assertSetParameterFails("internalQueryMaxAddToSetBytes", -1);

// Internal BSON max object size is slightly larger than the max user object size, to
// accommodate command metadata.
const bsonUserSizeLimit = assert.commandWorked(testDB.hello()).maxBsonObjectSize;
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

assertSetParameterSucceeds("internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals", 1001);
assertSetParameterFails("internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals", 0);
assertSetParameterFails("internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals", -1);

assertSetParameterSucceeds("internalQueryForceClassicEngine", true);
assertSetParameterSucceeds("internalQueryForceClassicEngine", false);

MongoRunner.stopMongod(conn);
})();
