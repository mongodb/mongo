/**
 * This test confirms that query stats store metrics fields for an update command (where the
 * update modification is specified as a replacement document or pipeline) are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [requires_fcv_90]
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {getQueryStatsUpdateCmd} from "jstests/libs/query/query_stats_utils.js";
import {
    assertWriteCmdQueryStatsSingleExec,
    describeRetryableWriteQueryStatsTests,
    describeWriteCmdQueryStatsReplicaSetTests,
    describeWriteCmdQueryStatsShardedTests,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";

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
            // Only the non-indexed field v is modified, so no index keys are touched.
            keysInserted: 0,
            keysDeleted: 0,
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
            // The replacement keeps _id unchanged and modifies only non-indexed fields, so no
            // index keys are touched.
            keysInserted: 0,
            keysDeleted: 0,
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
            // Only non-indexed fields are modified, so no index keys are touched.
            keysInserted: 0,
            keysDeleted: 0,
        },
    });
}

// An update that does not touch any indexed field performs no index maintenance, so keysInserted
// and keysDeleted are both 0. The collection has only the _id index and the updates never change _id.
function testNonIndexedFieldUpdate(testDB, coll, collName) {
    assert.commandWorked(
        testDB.runCommand({
            update: collName,
            updates: [{q: {v: {$lt: 3}}, u: {$set: {notIndexed: true}}, multi: true}],
            comment: "running non-indexed-field update!!",
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "update",
        keysExamined: 0,
        docsExamined: 8,
        writes: {
            nMatched: 2,
            nUpserted: 0,
            nModified: 2,
            nDeleted: 0,
            nInserted: 0,
            nUpdateOps: 1,
            nDeleteOps: 0,
            // No indexed field changed, so no index keys were inserted or deleted.
            keysInserted: 0,
            keysDeleted: 0,
        },
    });
}

// An update that changes a field covered by a secondary index performs index maintenance: the old
// index key is deleted and the new one is inserted. On a sharded cluster the shard reports these
// per-shard key counts in its write response and mongos sums them into the router-side entry, so
// this runs on both the replica-set and sharded suites.
function testIndexedFieldUpdate(testDB, coll, collName) {
    // The collection was reset (and any previous index dropped) in beforeEach. Build a secondary
    // index on the field 'w' and insert a single document carrying it. The index build and the
    // insert run before the update command, so they don't contribute to the update's metrics.
    assert.commandWorked(coll.createIndex({w: 1}));
    assert.commandWorked(coll.insert({_id: 9999, w: 1}));
    assert.commandWorked(
        testDB.runCommand({
            update: collName,
            // Filter on _id (IDHACK) rather than the mutated field, so the index scan is over the
            // _id index and the planner doesn't re-examine the {w: 1} index keys we are changing.
            updates: [{q: {_id: 9999}, u: {$set: {w: 2}}, multi: false}],
            comment: "running indexed-field update!!",
        }),
    );
    // The _id index targets the single document (keysExamined = docsExamined = 1). Changing the
    // indexed field w from 1 to 2 deletes the old {w: 1} index key and inserts the new {w: 2} key.
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
            keysInserted: 1,
            keysDeleted: 1,
        },
    });
    assert.commandWorked(coll.remove({_id: 9999}));
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
            // Only non-indexed fields are modified, so no index keys are touched.
            keysInserted: 0,
            keysDeleted: 0,
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

            it("should record zero key maintenance for a non-indexed-field update", function () {
                const {testDB, coll, collName} = ctxFn();
                testNonIndexedFieldUpdate(testDB, coll, collName);
            });

            it("should record key maintenance for an indexed-field update", function () {
                const {testDB, coll, collName} = ctxFn();
                testIndexedFieldUpdate(testDB, coll, collName);
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

describeWriteCmdQueryStatsShardedTests("query stats update command metrics (sharded)", (ctxFn) => {
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

    it("should record zero key maintenance for a non-indexed-field update", function () {
        const {testDB, coll, collName} = ctxFn();
        testNonIndexedFieldUpdate(testDB, coll, collName);
    });

    it("should record key maintenance for an indexed-field update", function () {
        const {testDB, coll, collName} = ctxFn();
        testIndexedFieldUpdate(testDB, coll, collName);
    });
});
