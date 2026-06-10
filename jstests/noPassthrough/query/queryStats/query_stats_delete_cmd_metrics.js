/**
 * This test confirms that query stats store metrics fields for a delete command are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [featureFlagQueryStatsDelete]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {getQueryStatsDeleteCmd} from "jstests/libs/query/query_stats_utils.js";
import {
    assertWriteCmdQueryStatsSingleExec,
    describeRetryableWriteQueryStatsTests,
    describeWriteCmdQueryStatsReplicaSetTests,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";

function testSingleDelete(testDB, coll, collName) {
    assert.commandWorked(
        testDB.runCommand({
            delete: collName,
            deletes: [{q: {v: {$lt: 3}}, limit: 1}],
            comment: "running single delete!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "delete",
        keysExamined: 0,
        docsExamined: 1,
        writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 1, nInserted: 0, nUpdateOps: 0, nDeleteOps: 1},
    });
}

// Simple _id queries skip parsing during normal delete processing (IDHACK optimization), but should still record
// metrics correctly.
function testIdDelete(testDB, coll, collName) {
    assert.commandWorked(coll.insert({_id: 999, v: 1}));
    assert.commandWorked(
        testDB.runCommand({
            delete: collName,
            deletes: [{q: {_id: 999}, limit: 1}],
            comment: "running delete filtered on _id!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "delete",
        keysExamined: 1,
        docsExamined: 1,
        writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 1, nInserted: 0, nUpdateOps: 0, nDeleteOps: 1},
    });
}

function testMultiDelete(testDB, coll, collName) {
    assert.commandWorked(
        testDB.runCommand({
            delete: collName,
            deletes: [{q: {v: {$gt: 4}}, limit: 0}],
            comment: "running multi delete!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "delete",
        keysExamined: 0,
        docsExamined: 8,
        writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 4, nInserted: 0, nUpdateOps: 0, nDeleteOps: 1},
    });
}

describeWriteCmdQueryStatsReplicaSetTests("query stats delete command metrics (replica set)", (ctxFn) => {
    describe("delete types", function () {
        it("should record single delete metrics", function () {
            const {testDB, coll, collName} = ctxFn();
            testSingleDelete(testDB, coll, collName);
        });

        it("should record simple _id delete metrics", function () {
            const {testDB, coll, collName} = ctxFn();
            testIdDelete(testDB, coll, collName);
        });

        it("should record multi delete metrics", function () {
            const {testDB, coll, collName} = ctxFn();
            testMultiDelete(testDB, coll, collName);
        });
    });

    // Test retryable writes with a filter on _id (IDHACK path)
    describeRetryableWriteQueryStatsTests("retried delete writes (_id filter)", ctxFn, {
        makeOp: (val) => ({q: {_id: val}, limit: 1}),
        opsField: "deletes",
        cmdName: "delete",
        getCount: (r) => r.n,
        getQueryStatsCmd: getQueryStatsDeleteCmd,
        assertDocModified: (coll, val) =>
            assert.eq(coll.findOne({_id: val}), null, "Document should be deleted after successful retry"),
    });

    // Non-_id filter exercises the regular query planner path (not IDHACK).
    describeRetryableWriteQueryStatsTests("retried delete writes (non-_id filter)", ctxFn, {
        makeOp: (val) => ({q: {a: val}, limit: 1}),
        opsField: "deletes",
        cmdName: "delete",
        getCount: (r) => r.n,
        getQueryStatsCmd: getQueryStatsDeleteCmd,
        assertDocModified: (coll, val) =>
            assert.eq(coll.findOne({a: val}), null, "Document should be deleted after successful retry"),
    });
});
