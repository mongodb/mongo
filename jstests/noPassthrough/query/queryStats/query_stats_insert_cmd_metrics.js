/**
 * This test confirms that query stats store metrics fields for an insert command are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [requires_fcv_90]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStatsInsertCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {
    assertShardInsertMetricsSingleExec,
    assertWriteCmdQueryStatsSingleExec,
    describeWriteCmdQueryStatsCrossShardTests,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kQueryStatsServerParams = {
    internalQueryStatsWriteCmdSampleRate: 1,
};

function testSingleDocInsert(testDB, coll, collName, shardConn = null) {
    assert.commandWorked(coll.deleteMany({}));
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [{v: 1}]}));
    // One _id index key is inserted per document. On a sharded cluster the shard reports this back
    // in its write response and mongos aggregates it into the router-side entry, so the count
    // matches the replica set case.
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "insert",
        keysExamined: 0,
        docsExamined: 0,
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 0,
            nInserted: 1,
            nUpdateOps: 0,
            nDeleteOps: 0,
            keysInserted: 1,
            keysDeleted: 0,
        },
    });

    if (shardConn) {
        // One _id index key inserted on the shard for the single inserted document.
        assertShardInsertMetricsSingleExec(shardConn, collName, {nInserted: 1, keysInserted: 1});
    }
}

function testMultiDocInsert(testDB, coll, collName, shardConn = null) {
    assert.commandWorked(coll.deleteMany({}));
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    assert.commandWorked(
        testDB.runCommand({insert: collName, documents: [{v: 1}, {v: 2}, {v: 3}]}),
    );
    // One _id index key is inserted per document, so three in total. The shell-generated ObjectId
    // _ids all sort into the same chunk (ObjectId sorts after numbers in BSON canonical type order,
    // so they fall in the upper chunk), so all three documents route to a single shard (shard0),
    // which reports keysInserted back to mongos for aggregation into the router-side entry.
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "insert",
        keysExamined: 0,
        docsExamined: 0,
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 0,
            nInserted: 3,
            nUpdateOps: 0,
            nDeleteOps: 0,
            keysInserted: 3,
            keysDeleted: 0,
        },
    });

    if (shardConn) {
        // One _id index key inserted on the shard for each of the three documents.
        assertShardInsertMetricsSingleExec(shardConn, collName, {nInserted: 3, keysInserted: 3});
    }
}

function assertPartialInsertQueryStats(testDB, coll, collName, nInserted, shardConn) {
    assertWriteCmdQueryStatsSingleExec(testDB, coll, {
        command: "insert",
        keysExamined: 0,
        docsExamined: 0,
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 0,
            nInserted: nInserted,
            nUpdateOps: 0,
            nDeleteOps: 0,
            keysInserted: nInserted,
            keysDeleted: 0,
        },
    });

    if (shardConn) {
        assertShardInsertMetricsSingleExec(shardConn, collName, {
            nInserted: nInserted,
            keysInserted: nInserted,
        });
    }
}

function testMultiDocsInsertPartialSuccess(testDB, coll, collName, shardConn = null) {
    assert.commandWorked(coll.deleteMany({}));
    // Pre-insert docs with explicit _ids so the second batch hits duplicate key errors on those docs.
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [{_id: 1}, {_id: 2}]}));

    // Tests ordered = false, which continues past errors.
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    if (shardConn) resetQueryStatsStore(shardConn, "1MB");
    testDB.runCommand({
        insert: collName,
        documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
        ordered: false,
    });
    assertPartialInsertQueryStats(testDB, coll, collName, 3 /*nInserted*/, shardConn);

    // Tests ordered = true, which halts the batch after the first failure.
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    if (shardConn) resetQueryStatsStore(shardConn, "1MB");
    testDB.runCommand({insert: collName, documents: [{_id: 6}, {_id: 1}, {_id: 7}], ordered: true});
    assertPartialInsertQueryStats(testDB, coll, collName, 1 /*nInserted*/, shardConn);
}

describe("query stats insert command metrics (replica set)", function () {
    let rst;
    let conn;
    let testDB;

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: kQueryStatsServerParams},
        });
        rst.startSet();
        rst.initiate();
        conn = rst.getPrimary();
        testDB = conn.getDB("test");
    });

    after(function () {
        rst?.stopSet();
    });

    beforeEach(function () {
        resetQueryStatsStore(conn, "1MB");
    });

    describe("insert types", function () {
        const collName = jsTestName() + "_metrics";
        let coll;

        before(function () {
            coll = testDB[collName];
        });

        it("should record single-doc insert metrics", function () {
            testSingleDocInsert(testDB, coll, collName);
        });

        it("should record multi-doc insert metrics", function () {
            testMultiDocInsert(testDB, coll, collName);
        });

        it("should record multi-doc insert metrics for partial successes", function () {
            testMultiDocsInsertPartialSuccess(testDB, coll, collName);
        });
    });

    // For retryable inserts (driven by lsid + txnNumber), execCount bumps once per insert
    // command dispatched, regardless of whether any document is actually written on that
    // invocation. This matches find's semantics (invocations counted) rather than update's
    // (per-statement — an update command can carry multiple ops). A command where every
    // stmtId is an already-executed retry still bumps execCount, because shape registration
    // and collection happen at the command boundary.
    describe("retried insert writes", function () {
        const collName = jsTestName() + "_retries";
        let coll;

        before(function () {
            coll = testDB[collName];
        });

        beforeEach(function () {
            coll.drop();
        });

        it("retried already-executed statements in a batch should not record query stats", function () {
            const lsid = {id: UUID()};
            const txnNumber = NumberLong(1);

            const firstCmd = {
                insert: collName,
                documents: [
                    {_id: 1, v: 1},
                    {_id: 2, v: 2},
                ],
                lsid: lsid,
                txnNumber: txnNumber,
            };

            const firstResult = assert.commandWorked(testDB.runCommand(firstCmd));
            assert.eq(firstResult.n, 2);

            let entries = getQueryStatsInsertCmd(conn, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after initial batch");
            // execCount counts insert commands, not documents — one command = one execution.
            // This is find-like (invocations) rather than update-like (per-statement/op).
            assert.eq(entries[0].metrics.execCount, 1);
            assert.eq(
                entries[0].metrics.writes.nInserted.sum,
                2,
                "nInserted.sum should be 2 (both docs actually written on first command)",
            );

            // Re-send with the same lsid/txnNumber but a 3-document batch. StmtIds 0 and 1
            // are already-executed retries; stmtId 2 is new. Because the server still processes
            // the retry command as a single invocation, execCount increments by 1.
            const retryCmd = {
                insert: collName,
                documents: [
                    {_id: 1, v: 1},
                    {_id: 2, v: 2},
                    {_id: 3, v: 3},
                ],
                lsid: lsid,
                txnNumber: txnNumber,
            };

            const retryResult = assert.commandWorked(testDB.runCommand(retryCmd));
            assert.eq(retryResult.retriedStmtIds, [0, 1], "Expected retriedStmtIds [0, 1]", {
                retriedStmtIds: retryResult.retriedStmtIds,
            });

            entries = getQueryStatsInsertCmd(conn, {collName: collName});
            assert.eq(entries.length, 1, "Expected still 1 query stats entry after partial retry");
            assert.eq(
                entries[0].metrics.execCount,
                2,
                "execCount should be 2 (one per insert command, regardless of doc or retry count)",
            );
            assert.eq(
                entries[0].metrics.writes.nInserted.sum,
                3,
                "nInserted.sum should be 3 (only stmtId 2 was newly inserted on partial retry)",
            );

            // Re-send the exact original two-document batch with the same lsid/txnNumber. Every
            // stmtId is an already-executed retry; nothing is newly inserted. execCount still
            // increments because collectQueryStatsMongod runs once per performInserts call at
            // the command boundary — find-style semantics (invocations counted), independent
            // of retry status of individual statements.
            const allRetryCmd = {
                insert: collName,
                documents: [
                    {_id: 1, v: 1},
                    {_id: 2, v: 2},
                ],
                lsid: lsid,
                txnNumber: txnNumber,
            };

            const allRetryResult = assert.commandWorked(testDB.runCommand(allRetryCmd));
            assert.eq(
                allRetryResult.retriedStmtIds,
                [0, 1],
                "Expected retriedStmtIds [0, 1] (every statement a retry)",
                {
                    retriedStmtIds: allRetryResult.retriedStmtIds,
                },
            );

            entries = getQueryStatsInsertCmd(conn, {collName: collName});
            assert.eq(
                entries.length,
                1,
                "Expected still 1 query stats entry after all-retry batch",
            );
            assert.eq(
                entries[0].metrics.execCount,
                3,
                "execCount should be 3 (command invocation counted even when no doc is newly inserted)",
            );
            assert.eq(
                entries[0].metrics.writes.nInserted.sum,
                3,
                "nInserted.sum should still be 3 (all-retry command inserts nothing new)",
            );
        });

        it("failed initial attempt should not record query stats; successful retry should", function () {
            const lsid = {id: UUID()};

            const insertCmd = {
                insert: collName,
                documents: [{_id: 1, v: 1}],
                lsid: lsid,
                txnNumber: NumberLong(1),
            };

            // Fail the first insert with a non-retryable error so the shell does not
            // transparently retry. The failpoint intercepts before the command handler runs,
            // so no query stats are registered for the failed attempt.
            const fp = configureFailPoint(
                conn,
                "failCommand",
                {
                    errorCode: ErrorCodes.OperationFailed,
                    failCommands: ["insert"],
                    namespace: "test." + collName,
                },
                {times: 1},
            );

            assert.commandFailedWithCode(testDB.runCommand(insertCmd), ErrorCodes.OperationFailed);
            assert.eq(coll.findOne({_id: 1}), null, "Document should not exist after failure");

            let entries = getQueryStatsInsertCmd(conn, {collName: collName});
            assert.eq(entries.length, 0, "Expected no query stats after failed attempt");

            // The failpoint has expired (times: 1). Retry with the same lsid and txnNumber —
            // the server never executed the statement, so it will treat this as a fresh
            // execution rather than an already-executed retry.
            const retryResult = assert.commandWorked(testDB.runCommand(insertCmd));
            assert.eq(retryResult.n, 1);
            assert.neq(coll.findOne({_id: 1}), null, "Document should exist after retry");

            entries = getQueryStatsInsertCmd(conn, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after successful retry");
            assert.eq(entries[0].metrics.execCount, 1);

            fp.off();
        });
    });
});

describeWriteCmdQueryStatsCrossShardTests(
    "query stats insert command metrics (sharded)",
    (ctxFn) => {
        it("should record single-doc insert metrics for a sharded collection", function () {
            const {testDB, coll, collName, st} = ctxFn();
            testSingleDocInsert(testDB, coll, collName, st.shard1);
        });

        it("should record multi-doc insert metrics for a sharded collection", function () {
            const {testDB, coll, collName, st} = ctxFn();
            testMultiDocInsert(testDB, coll, collName, st.shard1);
        });

        // Unlike updates and deletes, inserts always route to a single shard. No cross-shard fanout
        // is possible, so one shard is sufficient to for both replica set and sharded cluster tests.
        it("should record multi-doc insert metrics for partial successes for a sharded collection", function () {
            const {testDB, coll, collName, st} = ctxFn();
            testMultiDocsInsertPartialSuccess(testDB, coll, collName, st.shard1);
        });
    },
);
