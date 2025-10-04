/**
 * Tests whether the explain works for a single delete operation on a timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # To avoid multiversion tests
 *   requires_fcv_71,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getExecutionStages, getPlanStage} from "jstests/libs/query/analyze_plan.js";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const collNamePrefix = jsTestName() + "_";
let testCaseId = 0;

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const docs = [
    {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 1},
    {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: 1},
    {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: 2},
    {_id: 4, [timeFieldName]: dateTime, [metaFieldName]: 2},
];
const closedBucketFilter = {
    "control.closed": {$not: {$eq: true}},
};

function testDeleteExplain({
    singleDeleteOp,
    expectedDeleteStageName,
    expectedOpType = null,
    expectedBucketFilter,
    expectedResidualFilter = null,
    expectedNumDeleted,
    expectedNumUnpacked = null,
    expectedUsedIndexName = null,
}) {
    assert(expectedDeleteStageName === "TS_MODIFY" || expectedDeleteStageName === "DELETE");

    // Prepares a timeseries collection.
    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    coll.drop();

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    // Creates an index same as the one in the hint so as to verify that the index hint is honored.
    if (singleDeleteOp.hasOwnProperty("hint")) {
        assert.commandWorked(coll.createIndex(singleDeleteOp.hint));
    }

    assert.commandWorked(coll.insert(docs));

    // Verifies the TS_MODIFY stage in the plan.
    const innerDeleteCommand = {delete: coll.getName(), deletes: [singleDeleteOp]};
    const deleteExplainPlanCommand = {explain: innerDeleteCommand, verbosity: "queryPlanner"};
    let explain = assert.commandWorked(testDB.runCommand(deleteExplainPlanCommand));
    const deleteStage = getPlanStage(explain.queryPlanner.winningPlan, expectedDeleteStageName);
    assert.neq(null, deleteStage, `${expectedDeleteStageName} stage not found in the plan: ${tojson(explain)}`);
    if (expectedDeleteStageName === "TS_MODIFY") {
        assert.eq(expectedOpType, deleteStage.opType, `TS_MODIFY opType is wrong: ${tojson(deleteStage)}`);
        assert.eq(
            expectedBucketFilter,
            deleteStage.bucketFilter,
            `TS_MODIFY bucketFilter is wrong: ${tojson(deleteStage)}`,
        );
        assert.eq(
            expectedResidualFilter,
            deleteStage.residualFilter,
            `TS_MODIFY residualFilter is wrong: ${tojson(deleteStage)}`,
        );
    } else {
        const collScanStage = getPlanStage(explain.queryPlanner.winningPlan, "COLLSCAN");
        assert.neq(null, collScanStage, `COLLSCAN stage not found in the plan: ${tojson(explain)}`);
        assert.eq(expectedBucketFilter, collScanStage.filter, `COLLSCAN filter is wrong: ${tojson(collScanStage)}`);
    }

    if (expectedUsedIndexName) {
        const ixscanStage = getPlanStage(explain.queryPlanner.winningPlan, "IXSCAN");
        assert.eq(expectedUsedIndexName, ixscanStage.indexName, `Wrong index used: ${tojson(ixscanStage)}`);
    }

    // Verifies the TS_MODIFY stage in the execution stats.
    const deleteExplainStatsCommand = {explain: innerDeleteCommand, verbosity: "executionStats"};
    explain = assert.commandWorked(testDB.runCommand(deleteExplainStatsCommand));
    const execStages = getExecutionStages(explain);
    assert.gt(execStages.length, 0, `No execution stages found: ${tojson(explain)}`);
    assert.eq(
        expectedDeleteStageName,
        execStages[0].stage,
        `TS_MODIFY stage not found in executionStages: ${tojson(explain)}`,
    );
    if (expectedDeleteStageName === "TS_MODIFY") {
        assert.eq(
            expectedNumDeleted,
            execStages[0].nMeasurementsDeleted,
            `Got wrong nMeasurementsDeleted: ${tojson(execStages[0])}`,
        );
        assert.eq(
            expectedNumUnpacked,
            execStages[0].nBucketsUnpacked,
            `Got wrong nBucketsUnpacked: ${tojson(execStages[0])}`,
        );
    } else {
        assert.eq(expectedNumDeleted, execStages[0].nWouldDelete, `Got wrong nWouldDelete: ${tojson(execStages[0])}`);
    }

    assert.sameMembers(docs, coll.find().toArray(), "Explain command must not touch documents in the collection");
}

(function testDeleteManyWithEmptyQuery() {
    testDeleteExplain({
        singleDeleteOp: {
            q: {},
            limit: 0,
        },
        // If the delete query is empty, we should use the DELETE plan.
        expectedDeleteStageName: "DELETE",
        expectedOpType: "deleteMany",
        expectedBucketFilter: closedBucketFilter,
        expectedNumDeleted: 4,
    });
})();

(function testDeleteManyWithBucketFilter() {
    testDeleteExplain({
        singleDeleteOp: {
            // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is exactly 1.
            q: {[metaFieldName]: 2, _id: {$gte: 3}},
            limit: 0,
        },
        expectedDeleteStageName: "TS_MODIFY",
        expectedOpType: "deleteMany",
        expectedBucketFilter: {
            $and: [closedBucketFilter, {meta: {$eq: 2}}, {"control.max._id": {$_internalExprGte: 3}}],
        },
        expectedResidualFilter: {_id: {$gte: 3}},
        expectedNumDeleted: 2,
        expectedNumUnpacked: 1,
    });
})();

(function testDeleteManyWithBucketMetricFilterOnly() {
    testDeleteExplain({
        singleDeleteOp: {
            // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is exactly 1.
            q: {_id: {$lte: 3}},
            limit: 0,
        },
        expectedDeleteStageName: "TS_MODIFY",
        expectedOpType: "deleteMany",
        expectedBucketFilter: {$and: [closedBucketFilter, {"control.min._id": {$_internalExprLte: 3}}]},
        expectedResidualFilter: {_id: {$lte: 3}},
        expectedNumDeleted: 3,
        expectedNumUnpacked: 2,
    });
})();

(function testDeleteManyWithBucketFilterAndIndexHint() {
    testDeleteExplain({
        singleDeleteOp: {
            // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is exactly 1.
            q: {[metaFieldName]: 2, _id: 3},
            limit: 0,
            hint: {[metaFieldName]: 1},
        },
        expectedDeleteStageName: "TS_MODIFY",
        expectedOpType: "deleteMany",
        expectedBucketFilter: {
            $and: [
                closedBucketFilter,
                {meta: {$eq: 2}},
                {
                    $and: [{"control.min._id": {$_internalExprLte: 3}}, {"control.max._id": {$_internalExprGte: 3}}],
                },
            ],
        },
        expectedResidualFilter: {_id: {$eq: 3}},
        expectedNumDeleted: 1,
        expectedNumUnpacked: 1,
        expectedUsedIndexName: metaFieldName + "_1",
    });
})();

(function testDeleteOneWithEmptyBucketFilter() {
    testDeleteExplain({
        singleDeleteOp: {
            // The non-meta field filter leads to a COLLSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is 2.
            q: {_id: 3},
            limit: 1,
        },
        expectedDeleteStageName: "TS_MODIFY",
        expectedOpType: "deleteOne",
        expectedBucketFilter: {
            $and: [
                closedBucketFilter,
                {
                    $and: [{"control.min._id": {$_internalExprLte: 3}}, {"control.max._id": {$_internalExprGte: 3}}],
                },
            ],
        },
        expectedResidualFilter: {_id: {$eq: 3}},
        expectedNumDeleted: 1,
        expectedNumUnpacked: 1,
    });
})();

(function testDeleteOneWithBucketFilter() {
    testDeleteExplain({
        singleDeleteOp: {
            // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is exactly 1.
            q: {[metaFieldName]: 2, _id: {$gte: 1}},
            limit: 1,
        },
        expectedDeleteStageName: "TS_MODIFY",
        expectedOpType: "deleteOne",
        expectedBucketFilter: {
            $and: [closedBucketFilter, {meta: {$eq: 2}}, {"control.max._id": {$_internalExprGte: 1}}],
        },
        expectedResidualFilter: {_id: {$gte: 1}},
        expectedNumDeleted: 1,
        expectedNumUnpacked: 1,
    });
})();

(function testDeleteOneWithBucketFilterAndIndexHint() {
    testDeleteExplain({
        singleDeleteOp: {
            // The meta field filter leads to a FETCH/IXSCAN below the TS_MODIFY stage and so
            // 'expectedNumUnpacked' is exactly 1.
            q: {[metaFieldName]: 2, _id: {$gte: 1}},
            limit: 1,
            hint: {[metaFieldName]: 1},
        },
        expectedDeleteStageName: "TS_MODIFY",
        expectedOpType: "deleteOne",
        expectedBucketFilter: {
            $and: [closedBucketFilter, {meta: {$eq: 2}}, {"control.max._id": {$_internalExprGte: 1}}],
        },
        expectedResidualFilter: {_id: {$gte: 1}},
        expectedNumDeleted: 1,
        expectedNumUnpacked: 1,
        expectedUsedIndexName: metaFieldName + "_1",
    });
})();
