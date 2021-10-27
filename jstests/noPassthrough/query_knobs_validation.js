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
        internalQueryCacheEvictionRatio: 10.0,
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
        internalLookupStageIntermediateDocumentMaxSizeBytes: 100 * 1024 * 1024,
        internalQueryMaxPushBytes: 100 * 1024 * 1024,
        internalQueryMaxAddToSetBytes: 100 * 1024 * 1024,
        internalQueryMaxRangeBytes: 100 * 1024 * 1024,
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
        assert.commandFailed(testDB.adminCommand({setParameter: 1, [paramName]: value}));
    }

    // Verify that the default values are set as expected when the server starts up.
    assertDefaultParameterValues();

    assertSetParameterSucceeds("internalQueryCacheMaxSizeBytesBeforeStripDebugInfo", 1);
    assertSetParameterSucceeds("internalQueryCacheMaxSizeBytesBeforeStripDebugInfo", 0);
    assertSetParameterFails("internalQueryCacheMaxSizeBytesBeforeStripDebugInfo", -1);

    assertSetParameterSucceeds("internalQueryFacetMaxOutputDocSizeBytes", 1);
    assertSetParameterFails("internalQueryFacetMaxOutputDocSizeBytes", 0);
    assertSetParameterFails("internalQueryFacetMaxOutputDocSizeBytes", -1);

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

    MongoRunner.stopMongod(conn);
})();
