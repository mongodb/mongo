/**
 * This test confirms that query stats store metrics fields for an update command (where the
 * update modification is specified as a replacement document or pipeline) are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {
    assertWriteCmdQueryStatsSingleExec,
    describeRetryableWriteQueryStatsTests,
    describeWriteCmdQueryStatsReplicaSetTests,
    resetQueryStatsCollection,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function testReplacementUpdate(testDB, coll, collName) {
    assert.commandWorked(
        testDB.runCommand({
            update: collName,
            updates: [
                {
                    q: {$or: [{v: {$lt: 3}}, {v: {$eq: 4}}]},
                    u: {v: 1000, updated: true},
                    multi: false,
                },
            ],
            comment: "running replacement update!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "update",
        keysExamined: 0,
        docsExamined: 1,
        writes: {
            nMatched: 1,
            nUpserted: 0,
            nModified: 1,
            nDeleted: 0,
            nInserted: 0,
            nUpdateOps: 1,
            nDeleteOps: 0,
        },
    });
}

// Simple _id queries skip parsing during normal update processing (IDHACK optimization),
// but should still record metrics correctly.
function testIdUpdate(testDB, coll, collName) {
    assert.commandWorked(coll.insert({_id: 999, v: 1}));
    assert.commandWorked(
        testDB.runCommand({
            update: collName,
            updates: [{q: {_id: 999}, u: {_id: 999, v: 2000}, multi: false}],
            comment: "running update filtered on _id!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "update",
        keysExamined: 1,
        docsExamined: 1,
        writes: {
            nMatched: 1,
            nUpserted: 0,
            nModified: 1,
            nDeleted: 0,
            nInserted: 0,
            nUpdateOps: 1,
            nDeleteOps: 0,
        },
    });
    assert.commandWorked(coll.remove({_id: 999}));
}

function testModifierUpdate(testDB, coll, collName) {
    assert.commandWorked(
        testDB.runCommand({
            update: collName,
            updates: [
                {q: {}, u: {$set: {v: "newValue", documentUpdated: true, count: 42}}, multi: true},
            ],
            comment: "running modifier update!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "update",
        keysExamined: 0,
        docsExamined: 8,
        writes: {
            nMatched: 8,
            nUpserted: 0,
            nModified: 8,
            nDeleted: 0,
            nInserted: 0,
            nUpdateOps: 1,
            nDeleteOps: 0,
        },
    });
}

function testPipelineUpdate(testDB, coll, collName) {
    assert.commandWorked(
        testDB.runCommand({
            update: collName,
            updates: [
                {
                    q: {},
                    u: [
                        {$set: {v: "$$newValue", pipelineUpdated: true, count: 42}},
                        {$unset: "oldField"},
                        {$replaceWith: {newDoc: "$$ROOT", timestamp: "$$NOW", processed: true}},
                    ],
                    c: {newValue: 3000},
                    multi: true,
                },
            ],
            comment: "running pipeline update!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "update",
        keysExamined: 0,
        docsExamined: 8,
        writes: {
            nMatched: 8,
            nUpserted: 0,
            nModified: 8,
            nDeleted: 0,
            nInserted: 0,
            nUpdateOps: 1,
            nDeleteOps: 0,
        },
    });
}

describeWriteCmdQueryStatsReplicaSetTests(
    "query stats update command metrics (replica set)",
    (ctxFn) => {
        describe("update types", function () {
            it("should record replacement update metrics", function () {
                const {testDB, coll, collName} = ctxFn();
                testReplacementUpdate(testDB, coll, collName);
            });

            it("should record simple _id update metrics", function () {
                const {testDB, coll, collName} = ctxFn();
                testIdUpdate(testDB, coll, collName);
            });

            it("should record modifier update metrics", function () {
                const {testDB, coll, collName} = ctxFn();
                testModifierUpdate(testDB, coll, collName);
            });

            it("should record pipeline update metrics", function () {
                const {testDB, coll, collName} = ctxFn();
                testPipelineUpdate(testDB, coll, collName);
            });
        });

        // Test retryable writes with a filter on _id (IDHACK path)
        describeRetryableWriteQueryStatsTests("retried update writes (_id filter)", ctxFn, {
            makeOp: (val) => ({q: {_id: val}, u: {$set: {b: val * 100}}, multi: false}),
            opsField: "updates",
            cmdName: "update",
            getCount: (r) => r.nModified,
            getQueryStatsCmd: getQueryStatsUpdateCmd,
            assertDocModified: (coll, val) =>
                assert.eq(
                    coll.findOne({_id: val}).b,
                    val * 100,
                    "Document should be modified after successful retry",
                ),
        });

        // Non-_id filter exercises the regular query planner path (not IDHACK).
        describeRetryableWriteQueryStatsTests("retried update writes (non-_id filter)", ctxFn, {
            makeOp: (val) => ({q: {a: val}, u: {$set: {b: val * 100}}, multi: false}),
            opsField: "updates",
            cmdName: "update",
            getCount: (r) => r.nModified,
            getQueryStatsCmd: getQueryStatsUpdateCmd,
            assertDocModified: (coll, val) =>
                assert.eq(
                    coll.findOne({a: val}).b,
                    val * 100,
                    "Document should be modified after successful retry",
                ),
        });
    },
);

describe("query stats update command metrics (sharded)", function () {
    const collName = jsTestName() + "_sharded";
    let st;
    let testDB;
    let coll;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {
                setParameter: {internalQueryStatsWriteCmdSampleRate: 1},
            },
        });
        testDB = st.s.getDB("test");
        coll = testDB[collName];
        st.shardColl(coll, {_id: 1}, {_id: 1});
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        resetQueryStatsCollection(coll);
        resetQueryStatsStore(st.s, "1MB");
    });

    it("should record replacement update metrics", function () {
        testReplacementUpdate(testDB, coll, collName);
    });

    it("should record simple _id update metrics", function () {
        testIdUpdate(testDB, coll, collName);
    });

    it("should record modifier update metrics", function () {
        testModifierUpdate(testDB, coll, collName);
    });

    it("should record pipeline update metrics", function () {
        testPipelineUpdate(testDB, coll, collName);
    });
});
