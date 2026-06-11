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

import {isViewlessTimeseriesOnlySuite} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    getExecutionStages,
    getPlanStage,
    getPlanStages,
    getWinningPlanFromExplain,
    runExplainWithRoutingRetry,
} from "jstests/libs/query/analyze_plan.js";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
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

// In sharded clusters, the explain contains per-shard plans, so getPlanStages may return multiple
// matching stages (one per shard). Each shard's plan has the same structure, so we validate the
// first one. This helper finds the expected stage and returns it.
function getDeletePlanStage(explain, expectedDeleteStageName) {
    const winningPlan = getWinningPlanFromExplain(explain);
    const deleteStages = getPlanStages(winningPlan, expectedDeleteStageName);
    assert.gt(deleteStages.length, 0, `${expectedDeleteStageName} stage not found in the plan: ${tojson(explain)}`);
    return deleteStages[0];
}

function verifyQueryPlannerOutput(
    explain,
    {expectedDeleteStageName, expectedOpType, expectedBucketFilter, expectedResidualFilter, expectedUsedIndexName},
) {
    const deleteStage = getDeletePlanStage(explain, expectedDeleteStageName);

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
        const collScanStage = getPlanStage(deleteStage, "COLLSCAN");
        assert.neq(null, collScanStage, `COLLSCAN stage not found in the plan: ${tojson(explain)}`);
        assert.eq(expectedBucketFilter, collScanStage.filter, `COLLSCAN filter is wrong: ${tojson(collScanStage)}`);
    }

    if (expectedUsedIndexName) {
        const ixscanStage = getPlanStage(deleteStage, "IXSCAN");
        assert.neq(null, ixscanStage, `IXSCAN stage not found in plan: ${tojson(explain)}`);
        assert.eq(expectedUsedIndexName, ixscanStage.indexName, `Wrong index used: ${tojson(ixscanStage)}`);
    }
}

// In sharded clusters, execution stats contain per-shard results. Validate each stage name and
// sum metrics across shards.
function verifyExecutionStatsOutput(explain, {expectedDeleteStageName, expectedNumDeleted, expectedNumUnpacked}) {
    const execStages = getExecutionStages(explain);
    assert.gt(execStages.length, 0, `No execution stages found: ${tojson(explain)}`);

    let totalDeleted = 0;
    let totalUnpacked = 0;
    for (const stage of execStages) {
        assert.eq(
            expectedDeleteStageName,
            stage.stage,
            `Expected ${expectedDeleteStageName} stage in executionStages: ${tojson(explain)}`,
        );
        if (expectedDeleteStageName === "TS_MODIFY") {
            totalDeleted += stage.nMeasurementsDeleted;
            totalUnpacked += stage.nBucketsUnpacked;
        } else {
            totalDeleted += stage.nWouldDelete;
        }
    }
    assert.eq(expectedNumDeleted, totalDeleted, `Got wrong total deleted count: ${tojson(execStages)}`);
    if (expectedNumUnpacked !== null) {
        assert.eq(expectedNumUnpacked, totalUnpacked, `Got wrong nBucketsUnpacked: ${tojson(execStages)}`);
    }
}

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
    const collName = `${jsTestName()}_${testCaseId++}`;
    assert.commandWorked(
        testDB.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    let coll = testDB[collName];

    if (singleDeleteOp.hasOwnProperty("hint")) {
        assert.commandWorked(coll.createIndex(singleDeleteOp.hint));
    }

    assert.commandWorked(coll.insert(docs));

    const innerDeleteCommand = {delete: coll.getName(), deletes: [singleDeleteOp]};
    const expectations = {
        expectedDeleteStageName,
        expectedOpType,
        expectedBucketFilter,
        expectedResidualFilter,
        expectedNumDeleted,
        expectedNumUnpacked,
        expectedUsedIndexName,
    };

    const queryPlannerExplain = runExplainWithRoutingRetry(testDB, {
        explain: innerDeleteCommand,
        verbosity: "queryPlanner",
    });
    verifyQueryPlannerOutput(queryPlannerExplain, expectations);

    const executionStatsExplain = runExplainWithRoutingRetry(testDB, {
        explain: innerDeleteCommand,
        verbosity: "executionStats",
    });
    verifyExecutionStatsOutput(executionStatsExplain, expectations);

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
