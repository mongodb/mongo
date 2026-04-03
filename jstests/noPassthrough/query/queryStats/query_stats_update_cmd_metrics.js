/**
 * This test confirms that query stats store metrics fields for an update command (where the
 * update modification is specified as a replacement document or pipeline) are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [requires_fcv_90]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function resetCollection(coll) {
    coll.drop();
    assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]));
}

function testReplacementUpdate(testDB, coll, collName) {
    const cmd = {
        update: collName,
        updates: [
            {
                q: {$or: [{v: {$lt: 3}}, {v: {$eq: 4}}]},
                u: {v: 1000, updated: true},
                multi: false,
            },
        ],
        comment: "running replacement update!!",
    };

    assert.commandWorked(testDB.runCommand(cmd));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 1,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 1, nUpserted: 0, nModified: 1, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
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

// Simple _id queries skip parsing during normal update processing (IDHACK optimization),
// but should still record metrics correctly.
function testIdUpdate(testDB, coll, collName) {
    assert.commandWorked(coll.insert({_id: 999, v: 1}));

    const cmd = {
        update: collName,
        updates: [{q: {_id: 999}, u: {_id: 999, v: 2000}, multi: false}],
        comment: "running update filtered on _id!!",
    };

    assert.commandWorked(testDB.runCommand(cmd));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 1,
        docsExamined: 1,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 1, nUpserted: 0, nModified: 1, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
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

    assert.commandWorked(coll.remove({_id: 999}));
}

function testModifierUpdate(testDB, coll, collName) {
    const cmd = {
        update: collName,
        updates: [
            {
                q: {},
                u: {$set: {v: "newValue", documentUpdated: true, count: 42}},
                multi: true,
            },
        ],
        comment: "running modifier update!!",
    };

    assert.commandWorked(testDB.runCommand(cmd));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 8,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 8, nUpserted: 0, nModified: 8, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
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

function testPipelineUpdate(testDB, coll, collName) {
    const cmd = {
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
    };

    assert.commandWorked(testDB.runCommand(cmd));

    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, "update");

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 8,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 8, nUpserted: 0, nModified: 8, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
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

describe("query stats update command metrics (replica set)", function () {
    let rst;
    let conn;
    let testDB;

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    internalQueryStatsRateLimit: -1,
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
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

    describe("update types", function () {
        const collName = jsTestName() + "_metrics";
        let coll;

        before(function () {
            coll = testDB[collName];
        });

        beforeEach(function () {
            resetCollection(coll);
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

    // When retryable writes are active (indicated by the presence of a logical session ID and a
    // transaction ID), we should only record query stats when the write is actually executed,
    // even if it's retried several times.
    describe("retried update writes", function () {
        const collName = jsTestName() + "_retries";
        let coll;

        before(function () {
            coll = testDB[collName];
        });

        beforeEach(function () {
            coll.drop();
            assert.commandWorked(
                coll.insert([
                    {_id: 1, v: 1},
                    {_id: 2, v: 2},
                    {_id: 3, v: 3},
                ]),
            );
        });

        it("retried already-executed statements in a batch should not record query stats", function () {
            const lsid = {id: UUID()};
            const txnNumber = NumberLong(1);

            const firstCmd = {
                update: collName,
                updates: [
                    {q: {_id: 1}, u: {$set: {v: 100}}, multi: false},
                    {q: {_id: 2}, u: {$set: {v: 200}}, multi: false},
                ],
                lsid: lsid,
                txnNumber: txnNumber,
            };

            const firstResult = assert.commandWorked(testDB.runCommand(firstCmd));
            assert.eq(firstResult.nModified, 2);

            let entries = getQueryStatsUpdateCmd(conn, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after initial batch");
            assert.eq(entries[0].metrics.execCount, 2);

            // Re-send with the same lsid/txnNumber but a 3-statement batch. StmtIds 0 and 1
            // are already-executed retries; stmtId 2 is new.
            const retryCmd = {
                update: collName,
                updates: [
                    {q: {_id: 1}, u: {$set: {v: 100}}, multi: false},
                    {q: {_id: 2}, u: {$set: {v: 200}}, multi: false},
                    {q: {_id: 3}, u: {$set: {v: 300}}, multi: false},
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

            entries = getQueryStatsUpdateCmd(conn, {collName: collName});
            assert.eq(entries.length, 1, "Expected still 1 query stats entry after partial retry");
            assert.eq(
                entries[0].metrics.execCount,
                3,
                "execCount should be 3 (2 original + 1 new; retries not counted)",
            );
        });

        it("failed initial attempt should not record query stats; successful retry should", function () {
            const lsid = {id: UUID()};

            const updateCmd = {
                update: collName,
                updates: [{q: {_id: 2}, u: {$set: {v: 200}}, multi: false}],
                lsid: lsid,
                txnNumber: NumberLong(1),
            };

            // Fail the first update with a non-retryable error so the shell does not
            // transparently retry. The failpoint intercepts before the command handler runs,
            // so no query stats are registered for the failed attempt.
            const fp = configureFailPoint(
                conn,
                "failCommand",
                {
                    errorCode: ErrorCodes.OperationFailed,
                    failCommands: ["update"],
                    namespace: "test." + collName,
                },
                {times: 1},
            );

            assert.commandFailedWithCode(testDB.runCommand(updateCmd), ErrorCodes.OperationFailed);

            assert.eq(coll.findOne({_id: 2}).v, 2, "Document should not be modified after failed attempt");

            let entries = getQueryStatsUpdateCmd(conn, {collName: collName});
            assert.eq(entries.length, 0, "Expected no query stats after failed attempt");

            // The failpoint has expired (times: 1). Retry with the same lsid and txnNumber —
            // the server never executed the statement, so it will treat this as a fresh
            // execution rather than an already-executed retry.
            const retryResult = assert.commandWorked(testDB.runCommand(updateCmd));
            assert.eq(retryResult.nModified, 1);

            assert.eq(coll.findOne({_id: 2}).v, 200, "Document should be modified after successful retry");

            entries = getQueryStatsUpdateCmd(conn, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after successful retry");
            assert.eq(entries[0].metrics.execCount, 1);

            fp.off();
        });
    });
});

describe("query stats update command metrics (sharded)", function () {
    const collName = jsTestName() + "_sharded";
    let st;
    let testDB;
    let coll;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {
                setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsWriteCmdSampleRate: 1},
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
        resetCollection(coll);
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
