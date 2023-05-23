/**
 * Tests whether the explain works for a single update operation on a timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # To avoid multiversion tests
 *   requires_fcv_71,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   # TODO SERVER-66393 Remove this tag.
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO SERVER-73726 Remove this tag.
 *   assumes_unsharded_collection,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanStage() and getExecutionStages().
load("jstests/core/timeseries/libs/timeseries_writes_util.js");

const dateTime = ISODate("2021-07-12T16:00:00Z");

const testDB = getTestDB();

const docs = [
    {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 1},
    {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: 1},
    {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: 2},
    {_id: 4, [timeFieldName]: dateTime, [metaFieldName]: 2},
];

function testUpdateExplain({
    singleUpdateOp,
    expectedUpdateStageName,
    expectedOpType = null,
    expectedBucketFilter,
    expectedResidualFilter = null,
    expectedNumUpdated,
    expectedNumMatched = expectedNumUpdated,
    expectedNumUpserted = 0,
    expectedNumUnpacked = null,
    expectedUsedIndexName = null
}) {
    assert(expectedUpdateStageName === "TS_MODIFY" || expectedUpdateStageName === "UPDATE");

    // Prepares a timeseries collection.
    const collName = getCallerName();
    const coll = testDB.getCollection(collName);
    prepareCollection({collName, initialDocList: docs});

    // Creates an index same as the one in the hint so as to verify that the index hint is honored.
    if (singleUpdateOp.hasOwnProperty("hint")) {
        assert.commandWorked(coll.createIndex(singleUpdateOp.hint));
    }

    // Verifies the TS_MODIFY stage in the plan.
    const innerUpdateCommand = {update: coll.getName(), updates: [singleUpdateOp]};
    const updateExplainPlanCommand = {explain: innerUpdateCommand, verbosity: "queryPlanner"};
    let explain = assert.commandWorked(testDB.runCommand(updateExplainPlanCommand));
    const updateStage = getPlanStage(explain.queryPlanner.winningPlan, expectedUpdateStageName);
    assert.neq(null,
               updateStage,
               `${expectedUpdateStageName} stage not found in the plan: ${tojson(explain)}`);
    if (expectedUpdateStageName === "TS_MODIFY") {
        assert.eq(expectedOpType,
                  updateStage.opType,
                  `TS_MODIFY opType is wrong: ${tojson(updateStage)}`);
        assert.eq(expectedBucketFilter,
                  updateStage.bucketFilter,
                  `TS_MODIFY bucketFilter is wrong: ${tojson(updateStage)}`);
        assert.eq(expectedResidualFilter,
                  updateStage.residualFilter,
                  `TS_MODIFY residualFilter is wrong: ${tojson(updateStage)}`);
    } else {
        const collScanStage = getPlanStage(explain.queryPlanner.winningPlan, "COLLSCAN");
        assert.neq(null, collScanStage, `COLLSCAN stage not found in the plan: ${tojson(explain)}`);
        assert.eq(expectedBucketFilter,
                  collScanStage.filter,
                  `COLLSCAN filter is wrong: ${tojson(collScanStage)}`);
    }

    if (expectedUsedIndexName) {
        const ixscanStage = getPlanStage(explain.queryPlanner.winningPlan, "IXSCAN");
        assert.eq(expectedUsedIndexName,
                  ixscanStage.indexName,
                  `Wrong index used: ${tojson(ixscanStage)}`);
    }

    // Verifies the TS_MODIFY stage in the execution stats.
    const updateExplainStatsCommand = {explain: innerUpdateCommand, verbosity: "executionStats"};
    explain = assert.commandWorked(testDB.runCommand(updateExplainStatsCommand));
    const execStages = getExecutionStages(explain);
    assert.gt(execStages.length, 0, `No execution stages found: ${tojson(explain)}`);
    assert.eq(expectedUpdateStageName,
              execStages[0].stage,
              `TS_MODIFY stage not found in executionStages: ${tojson(explain)}`);
    if (expectedUpdateStageName === "TS_MODIFY") {
        assert.eq(expectedNumUpdated,
                  execStages[0].nMeasurementsUpdated,
                  `Got wrong nMeasurementsUpdated: ${tojson(execStages[0])}`);
        assert.eq(expectedNumMatched,
                  execStages[0].nMeasurementsMatched,
                  `Got wrong nMeasurementsMatched: ${tojson(execStages[0])}`);
        assert.eq(expectedNumUpserted,
                  execStages[0].nMeasurementsUpserted,
                  `Got wrong nMeasurementsUpserted: ${tojson(execStages[0])}`);
        assert.eq(expectedNumUnpacked,
                  execStages[0].nBucketsUnpacked,
                  `Got wrong nBucketsUnpacked: ${tojson(execStages[0])}`);
    } else {
        assert.eq(expectedNumUpdated,
                  execStages[0].nWouldModify,
                  `Got wrong nWouldModify: ${tojson(execStages[0])}`);
        assert.eq(expectedNumMatched,
                  execStages[0].nMatched,
                  `Got wrong nMatched: ${tojson(execStages[0])}`);
    }

    assert.sameMembers(
        docs, coll.find().toArray(), "Explain command must not touch documents in the collection");
}

(function testUpdateManyWithEmptyQuery() {
    testUpdateExplain({
        singleUpdateOp: {
            q: {},
            u: {$set: {[metaFieldName]: 3}},
            multi: true,
        },
        // If the update query is empty, we should use the UPDATE plan.
        expectedUpdateStageName: "UPDATE",
        expectedOpType: "updateMany",
        expectedBucketFilter: makeBucketFilter(),
        expectedNumUpdated: 4,
    });
})();

(function testUpdateManyWithBucketFilter() {
    testUpdateExplain({
        singleUpdateOp: {
            // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is exactly 1.
            q: {[metaFieldName]: 2, _id: {$gte: 3}},
            u: {$set: {[metaFieldName]: 2}},
            multi: true,
        },
        expectedUpdateStageName: "TS_MODIFY",
        expectedOpType: "updateMany",
        expectedBucketFilter:
            makeBucketFilter({meta: {$eq: 2}}, {"control.max._id": {$_internalExprGte: 3}}),
        expectedResidualFilter: {_id: {$gte: 3}},
        expectedNumUpdated: 0,
        expectedNumMatched: 2,
        expectedNumUnpacked: 1
    });
})();

(function testUpdateManyWithBucketFilterAndIndexHint() {
    testUpdateExplain({
        singleUpdateOp: {
            // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is exactly 1.
            q: {[metaFieldName]: 2, _id: 3},
            u: {$set: {[metaFieldName]: 3}},
            multi: true,
            hint: {[metaFieldName]: 1}
        },
        expectedUpdateStageName: "TS_MODIFY",
        expectedOpType: "updateMany",
        expectedBucketFilter: makeBucketFilter({meta: {$eq: 2}}, {
            $and: [
                {"control.min._id": {$_internalExprLte: 3}},
                {"control.max._id": {$_internalExprGte: 3}}
            ]
        }),
        expectedResidualFilter: {_id: {$eq: 3}},
        expectedNumUpdated: 1,
        expectedNumUnpacked: 1,
        expectedUsedIndexName: metaFieldName + "_1"
    });
})();

(function testUpsert() {
    testUpdateExplain({
        singleUpdateOp: {
            q: {[metaFieldName]: 100},
            u: {$set: {[timeFieldName]: dateTime}},
            multi: true,
            upsert: true,
        },
        expectedUpdateStageName: "TS_MODIFY",
        expectedOpType: "updateMany",
        expectedBucketFilter: makeBucketFilter({meta: {$eq: 100}}),
        expectedResidualFilter: {},
        expectedNumUpdated: 0,
        expectedNumMatched: 0,
        expectedNumUnpacked: 0,
        expectedNumUpserted: 1,
    });
})();

(function testUpsertNoop() {
    testUpdateExplain({
        singleUpdateOp: {
            q: {[metaFieldName]: 1},
            u: {$set: {f: 10}},
            multi: true,
            upsert: true,
        },
        expectedUpdateStageName: "TS_MODIFY",
        expectedOpType: "updateMany",
        expectedBucketFilter: makeBucketFilter({meta: {$eq: 1}}),
        expectedResidualFilter: {},
        expectedNumUpdated: 2,
        expectedNumMatched: 2,
        expectedNumUnpacked: 1,
        expectedNumUpserted: 0,
    });
})();

// TODO SERVER-73726 Reevaluate whether this exclusion is needed.
if (FeatureFlagUtil.isPresentAndEnabled(db, "UpdateOneWithoutShardKey")) {
    (function testUpdateOneWithEmptyBucketFilter() {
        testUpdateExplain({
            singleUpdateOp: {
                // The non-meta field filter leads to a COLLSCAN below the TS_MODIFY stage and so
                // 'expectedNumUnpacked' is 2.
                q: {_id: 3},
                u: {$set: {[metaFieldName]: 3}},
                multi: false,
            },
            expectedUpdateStageName: "TS_MODIFY",
            expectedOpType: "updateOne",
            expectedBucketFilter: makeBucketFilter({
                $and: [
                    {"control.min._id": {$_internalExprLte: 3}},
                    {"control.max._id": {$_internalExprGte: 3}}
                ]
            }),
            expectedResidualFilter: {_id: {$eq: 3}},
            expectedNumUpdated: 1,
            expectedNumUnpacked: 1
        });
    })();

    (function testUpdateOneWithBucketFilter() {
        testUpdateExplain({
            singleUpdateOp: {
                // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
                // 'expectedNumUnpacked' is exactly 1.
                q: {[metaFieldName]: 2, _id: {$gte: 1}},
                u: {$set: {[metaFieldName]: 3}},
                multi: false,
            },
            expectedUpdateStageName: "TS_MODIFY",
            expectedOpType: "updateOne",
            expectedBucketFilter:
                makeBucketFilter({meta: {$eq: 2}}, {"control.max._id": {$_internalExprGte: 1}}),
            expectedResidualFilter: {_id: {$gte: 1}},
            expectedNumUpdated: 1,
            expectedNumUnpacked: 1
        });
    })();

    (function testUpdateOneWithBucketFilterAndIndexHint() {
        testUpdateExplain({
            singleUpdateOp: {
                // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
                // 'expectedNumUnpacked' is exactly 1.
                q: {[metaFieldName]: 2, _id: {$gte: 1}},
                u: {$set: {[metaFieldName]: 3}},
                multi: false,
                hint: {[metaFieldName]: 1}
            },
            expectedUpdateStageName: "TS_MODIFY",
            expectedOpType: "updateOne",
            expectedBucketFilter:
                makeBucketFilter({meta: {$eq: 2}}, {"control.max._id": {$_internalExprGte: 1}}),
            expectedResidualFilter: {_id: {$gte: 1}},
            expectedNumUpdated: 1,
            expectedNumUnpacked: 1,
            expectedUsedIndexName: metaFieldName + "_1"
        });
    })();
}
})();
