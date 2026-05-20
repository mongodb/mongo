/**
 * This test confirms that query stats store metrics fields for an insert command are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [featureFlagQueryStatsInsert]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStatsInsertCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kQueryStatsServerParams = {
    internalQueryStatsWriteCmdSampleRate: 1,
};

function testSingleDocInsert(testDB, coll, collName) {
    coll.drop();

    const cmd = {
        insert: collName,
        documents: [{v: 1}],
    };

    assert.commandWorked(testDB.runCommand(cmd));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "insert");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 0,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 0, nInserted: 1, nUpdateOps: 0},
    });
    assertExpectedResults({
        results: entry,
        expectedQueryStatsKey: entry.key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 0,
        expectedDocsReturnedMax: 0,
        expectedDocsReturnedMin: 0,
        expectedDocsReturnedSumOfSq: 0,
    });
}

function testMultiDocInsert(testDB, coll, collName) {
    coll.drop();

    const cmd = {
        insert: collName,
        documents: [{v: 1}, {v: 2}, {v: 3}],
    };

    assert.commandWorked(testDB.runCommand(cmd));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "insert");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 0,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 0, nInserted: 3, nUpdateOps: 0},
    });
    assertExpectedResults({
        results: entry,
        expectedQueryStatsKey: entry.key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 0,
        expectedDocsReturnedMax: 0,
        expectedDocsReturnedMin: 0,
        expectedDocsReturnedSumOfSq: 0,
    });
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
            assert.eq(
                retryResult.retriedStmtIds,
                [0, 1],
                "Expected retriedStmtIds [0, 1]: " + tojson(retryResult.retriedStmtIds),
            );

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
                "Expected retriedStmtIds [0, 1] (every statement a retry): " + tojson(allRetryResult.retriedStmtIds),
            );

            entries = getQueryStatsInsertCmd(conn, {collName: collName});
            assert.eq(entries.length, 1, "Expected still 1 query stats entry after all-retry batch");
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

// TODO SERVER-122076: Enable once query stats collection for inserts is wired through the
// sharded write path. Currently this branch only implements collection for standalone/replica set.
describe.skip("query stats insert command metrics (sharded)", function () {
    const collName = jsTestName() + "_sharded";
    let st;
    let testDB;
    let coll;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: kQueryStatsServerParams},
        });
        testDB = st.s.getDB("test");
        coll = testDB[collName];
        st.shardColl(coll, {_id: 1}, {_id: 0});
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        resetQueryStatsStore(st.s, "1MB");
    });

    it("should record single-doc insert metrics", function () {
        testSingleDocInsert(testDB, coll, collName);
    });

    it("should record multi-doc insert metrics", function () {
        testMultiDocInsert(testDB, coll, collName);
    });
});
