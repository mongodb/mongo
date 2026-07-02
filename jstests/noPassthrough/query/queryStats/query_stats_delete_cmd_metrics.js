/**
 * This test confirms that query stats store metrics fields for a delete command are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [requires_fcv_90]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    getQueryStatsDeleteCmd,
    getLatestQueryStatsEntry,
    getQueryExecMetrics,
} from "jstests/libs/query/query_stats_utils.js";
import {
    assertWriteCmdQueryStatsSingleExec,
    describeRetryableWriteQueryStatsTests,
    describeWriteCmdQueryStatsCrossShardTests,
    describeWriteCmdQueryStatsReplicaSetTests,
    describeWriteCmdQueryStatsShardedTests,
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
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 1,
            nInserted: 0,
            nUpdateOps: 0,
            nDeleteOps: 1,
            // Deleting one document removes its single _id index key.
            keysInserted: 0,
            keysDeleted: 1,
        },
    });
}

// Simple _id queries skip parsing during normal delete processing (IDHACK optimization), but should
// still record metrics correctly.
function testIdDelete(testDB, coll, collName) {
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
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 1,
            nInserted: 0,
            nUpdateOps: 0,
            nDeleteOps: 1,
            // Deleting one document removes its single _id index key.
            keysInserted: 0,
            keysDeleted: 1,
        },
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
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 4,
            nInserted: 0,
            nUpdateOps: 0,
            nDeleteOps: 1,
            // Deleting four documents removes one _id index key each.
            keysInserted: 0,
            keysDeleted: 4,
        },
    });
}

function testDeleteNoMatches(testDB, coll, collName) {
    assert.commandWorked(
        testDB.runCommand({
            delete: collName,
            deletes: [{q: {v: {$gt: 100}}, limit: 0}],
        }),
    );
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "delete",
        keysExamined: 0,
        docsExamined: 8,
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 0,
            nInserted: 0,
            nUpdateOps: 0,
            nDeleteOps: 1,
            keysInserted: 0,
            keysDeleted: 0,
        },
    });
}

function testMultiDeletePartialSuccess(testDB, coll, collName, conn) {
    // First two delete operations should fail, third succeeds.
    const fp = configureFailPoint(conn, "failAllRemoves", {}, {times: 2});
    testDB.runCommand({
        delete: collName,
        deletes: [
            {q: {v: {$gt: 5}}, limit: 0},
            {q: {v: {$gt: 6}}, limit: 0},
            {q: {v: {$lte: 1}}, limit: 0},
        ],
        ordered: false,
    });
    fp.off();

    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "delete",
        keysExamined: 0,
        docsExamined: 8,
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 1,
            nInserted: 0,
            nUpdateOps: 0,
            nDeleteOps: 3,
            keysInserted: 0,
            keysDeleted: 1,
        },
    });
}

describeWriteCmdQueryStatsReplicaSetTests(
    "query stats delete command metrics (replica set)",
    (ctxFn) => {
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

            it("should record delete metrics when no documents match the filter", function () {
                const {testDB, coll, collName} = ctxFn();
                testDeleteNoMatches(testDB, coll, collName);
            });

            it("should record multi delete metrics for partial successes", function () {
                const {testDB, coll, collName} = ctxFn();
                testMultiDeletePartialSuccess(testDB, coll, collName, testDB.getMongo());
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
                assert.eq(
                    coll.findOne({_id: val}),
                    null,
                    "Document should be deleted after successful retry",
                ),
        });

        // Non-_id filter exercises the regular query planner path (not IDHACK).
        describeRetryableWriteQueryStatsTests("retried delete writes (non-_id filter)", ctxFn, {
            makeOp: (val) => ({q: {a: val}, limit: 1}),
            opsField: "deletes",
            cmdName: "delete",
            getCount: (r) => r.n,
            getQueryStatsCmd: getQueryStatsDeleteCmd,
            assertDocModified: (coll, val) =>
                assert.eq(
                    coll.findOne({a: val}),
                    null,
                    "Document should be deleted after successful retry",
                ),
        });

        describe("skip unsupported command types", function () {
            it("should not record delete stats for findAndModify with remove", function () {
                const {testDB, coll, collName, conn} = ctxFn();
                assert.commandWorked(
                    testDB.runCommand({findAndModify: collName, query: {v: 1}, remove: true}),
                );
                assert.eq(
                    0,
                    getQueryStatsDeleteCmd(conn, {collName}).length,
                    "Expected no delete stats for findAndModify remove",
                );
            });

            it("should not have queryShapeHash in explain for findAndModify with remove", function () {
                const {testDB, coll} = ctxFn();
                const explainResult = assert.commandWorked(
                    coll.explain().findAndModify({
                        query: {v: 1},
                        remove: true,
                    }),
                );
                assert(
                    !explainResult.hasOwnProperty("queryShapeHash"),
                    "Expected no queryShapeHash in explain for findAndModify with remove",
                    {explainResult},
                );
            });

            it("should not record delete stats for bulkWrite delete op", function () {
                const {testDB, coll, collName, conn} = ctxFn();
                assert.commandWorked(
                    testDB.adminCommand({
                        bulkWrite: 1,
                        ops: [{delete: 0, filter: {v: 2}, multi: false}],
                        nsInfo: [{ns: coll.getFullName()}],
                    }),
                );
                assert.eq(
                    0,
                    getQueryStatsDeleteCmd(conn, {collName}).length,
                    "Expected no delete stats for bulkWrite delete",
                );
            });

            it("should not have queryShapeHash in explain for bulkWrite delete op", function () {
                const {testDB, coll} = ctxFn();
                const explainResult = assert.commandWorked(
                    testDB.adminCommand({
                        explain: {
                            bulkWrite: 1,
                            ops: [{delete: 0, filter: {v: 2}, multi: false}],
                            nsInfo: [{ns: coll.getFullName()}],
                        },
                    }),
                );
                assert(
                    !explainResult.hasOwnProperty("queryShapeHash"),
                    "Expected no queryShapeHash in explain for bulkWrite delete",
                    {explainResult},
                );
            });

            it("should not record delete stats for applyOps delete op", function () {
                const {testDB, coll, collName, conn} = ctxFn();
                assert.commandWorked(coll.insert({_id: "applyOpsDeleteTest"}));
                assert.commandWorked(
                    testDB.adminCommand({
                        applyOps: [
                            {op: "d", ns: coll.getFullName(), o: {_id: "applyOpsDeleteTest"}},
                        ],
                    }),
                );
                assert.eq(
                    0,
                    getQueryStatsDeleteCmd(conn, {collName}).length,
                    "Expected no delete stats for applyOps delete",
                );
            });
        });
    },
);

describeWriteCmdQueryStatsShardedTests("query stats delete command metrics (sharded)", (ctxFn) => {
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

        it("should record multi delete metrics when no documents match the filter", function () {
            const {testDB, coll, collName, st} = ctxFn();
            testDeleteNoMatches(testDB, coll, collName);
        });
    });
});

function validateDocsExaminedMetric(docsExamined, numDocs, entry) {
    // On sharded clusters, deletes can double-count docsExamined: the COLLSCAN phase counts each
    // matching document once, and then the DELETE stage could re-fetch it from the shard, counting
    // it a second time. So docsExamined may be anywhere in [expected, expected * 2].
    assert.gte(docsExamined, numDocs, "docsExamined is smaller than expected", {
        entry,
    });
    assert.lte(docsExamined, numDocs * 2, "docsExamined is larger than expected", {
        entry,
    });
}

// Tests cross-shard partial success, where shard1's delete fails, while shard0's delete succeeds for the
// same operation. Asserts mongos's partial shard aggregation results and shard-level stats independently.
describeWriteCmdQueryStatsCrossShardTests(
    "query stats delete command metrics (sharded, cross-shard partial success)",
    (ctxFn) => {
        it("should record partial success when shard0 delete succeeds and shard1 delete fails", function () {
            const {st, testDB, coll, collName} = ctxFn();
            const numShard0Docs = 2;
            assert.commandWorked(
                coll.insert([
                    {_id: -1, v: 4},
                    {_id: -2, v: 5},
                ]),
            );
            assert.commandWorked(
                coll.insert([
                    {_id: 1, v: 6},
                    {_id: 2, v: 7},
                ]),
            );

            const fp = configureFailPoint(st.shard1, "failAllRemoves", {}, {times: 1});
            testDB.runCommand({
                delete: collName,
                deletes: [{q: {v: {$gt: 3}}, limit: 0}],
                ordered: false,
            });
            fp.off();

            // shard0 should succeed, examining and deleting 2 docs.
            const shard0Entries = getQueryStatsDeleteCmd(st.shard0, {collName});
            assert.eq(shard0Entries.length, 1, "Expected shard0 to have one query stats entry", {
                shard0Entries,
            });

            const shardDocsExamined = getQueryExecMetrics(shard0Entries[0].metrics).docsExamined
                .sum;
            validateDocsExaminedMetric(shardDocsExamined, numShard0Docs, shard0Entries[0]);
            //  To make assertAggregatedMetricsSingleExec pass, we pass in the actual docsExamined value.
            assertAggregatedMetricsSingleExec(shard0Entries[0], {
                keysExamined: 0,
                docsExamined: shardDocsExamined,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 2,
                    nInserted: 0,
                    nUpdateOps: 0,
                    nDeleteOps: 1,
                    keysInserted: 0,
                    keysDeleted: 2,
                },
            });

            // shard1 should fail, resulting in no stats.
            const shard1Entries = getQueryStatsDeleteCmd(st.shard1, {collName});
            assert.eq(shard1Entries.length, 0, "Expected shard1 to have no query stats entry", {
                shard1Entries,
            });

            // mongos shows aggregated stats (equivalent to shard0's stats).
            const mongosEntry = getLatestQueryStatsEntry(st.s, {collName: coll.getName()});

            const mongosDocsExamined = getQueryExecMetrics(mongosEntry.metrics).docsExamined.sum;
            validateDocsExaminedMetric(mongosDocsExamined, numShard0Docs, mongosEntry);
            assertAggregatedMetricsSingleExec(mongosEntry, {
                keysExamined: 0,
                docsExamined: mongosDocsExamined,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 2,
                    nInserted: 0,
                    nUpdateOps: 0,
                    nDeleteOps: 1,
                    keysInserted: 0,
                    keysDeleted: 2,
                },
            });
        });
    },
);
