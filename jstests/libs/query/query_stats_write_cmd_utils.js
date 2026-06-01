/**
 * Shared test helpers for write command (insert/update/delete) query stats tests.
 * Import this file for mocha-style metrics tests and one-way tokenization tests.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

/**
 * Asserts that the most recent query stats entry for `coll` matches a single-execution write
 * command. Callers supply only the fields that vary per test; the boolean planner flags all
 * default to false and the docs-returned metrics default to 0.
 */
export function assertWriteCmdQueryStatsSingleExec(testDB, coll, {command, keysExamined, docsExamined, writes}) {
    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, command);
    assertAggregatedMetricsSingleExec(entry, {
        keysExamined,
        docsExamined,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes,
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

/**
 * Drops and re-populates a collection with 8 documents {v: 1} through {v: 8}, used as a standard
 * starting state for write command query stats tests.
 */
export function resetQueryStatsCollection(coll) {
    coll.drop();
    assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]));
}

/**
 * Creates a standard write command query stats test suite on a single-node replica set.
 * Sets up the ReplSet fixture and query stats store / collection reset in hooks, then calls
 * bodyFn with a ctxFn accessor so inner tests can reach {conn, testDB, coll, collName}.
 *
 * @param {string} label - outer describe label
 * @param {Function} bodyFn - function(ctxFn) where ctxFn() => {conn, testDB, coll, collName}
 */
export function describeWriteCmdQueryStatsReplicaSetTests(label, bodyFn) {
    describe(label, function () {
        let rst, conn, testDB, coll;
        const collName = jsTestName() + "_metrics";

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
            coll = testDB[collName];
        });

        after(function () {
            rst?.stopSet();
        });

        beforeEach(function () {
            resetQueryStatsStore(conn, "1MB");
            resetQueryStatsCollection(coll);
        });

        bodyFn(() => ({conn, testDB, coll, collName}));
    });
}

/**
 * Describes the retryable write query stats tests inside the current describe block:
 * (1) retried already-executed statements should not double-count execCount,
 * (2) a failed attempt should not record stats; the successful retry should.
 *
 * @param {string} label - inner describe label
 * @param {Function} ctxFn - function() => {conn, testDB}
 * @param {object} opts
 *   makeOp(val)            - builds one write op with a given argument
 *   opsField               - "deletes" | "updates"
 *   cmdName                - "delete" | "update"
 *   getCount(result)       - extracts the write count from a result
 *   getQueryStatsCmd       - e.g. getQueryStatsDeleteCmd
 *   assertDocModified(coll, id) - asserts expected doc state after a successful op
 */
export function describeRetryableWriteQueryStatsTests(
    label,
    ctxFn,
    {makeOp, opsField, cmdName, getCount, getQueryStatsCmd, assertDocModified},
) {
    describe(label, function () {
        const collName = jsTestName() + "_retries";
        let coll;

        before(function () {
            coll = ctxFn().testDB[collName];
        });

        beforeEach(function () {
            coll.drop();
            assert.commandWorked(
                coll.insert([
                    {_id: 1, a: 1, b: "abc"},
                    {_id: 2, a: 2, b: "def"},
                    {_id: 3, a: 3, b: "geh"},
                ]),
            );
        });

        // When retryable writes are active (indicated by the presence of a logical session ID and a transaction ID),
        // we should only record query stats when the write is actually executed, even if it's retried several times.

        it("retried already-executed statements in a batch should not record query stats", function () {
            const {conn, testDB} = ctxFn();
            const lsid = {id: UUID()};
            const txnNumber = NumberLong(1);

            const firstResult = assert.commandWorked(
                testDB.runCommand({
                    [cmdName]: collName,
                    [opsField]: [makeOp(1), makeOp(2)],
                    lsid,
                    txnNumber,
                }),
            );
            assert.eq(getCount(firstResult), 2);

            let entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after initial batch");
            assert.eq(entries[0].metrics.execCount, 2);

            const retryResult = assert.commandWorked(
                testDB.runCommand({
                    [cmdName]: collName,
                    [opsField]: [makeOp(1), makeOp(2), makeOp(3)],
                    lsid,
                    txnNumber,
                }),
            );
            assert.eq(retryResult.retriedStmtIds, [0, 1], "Expected retriedStmtIds [0, 1]", {
                retriedStmtIds: retryResult.retriedStmtIds,
            });

            entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 1, "Expected still 1 query stats entry after partial retry");
            assert.eq(
                entries[0].metrics.execCount,
                3,
                "execCount should be 3 (2 original + 1 new; retries not counted)",
            );
        });

        it("failed initial attempt should not record query stats; successful retry should", function () {
            const {conn, testDB} = ctxFn();
            const lsid = {id: UUID()};
            const cmd = {
                [cmdName]: collName,
                [opsField]: [makeOp(2)],
                lsid,
                txnNumber: NumberLong(1),
            };

            const fp = configureFailPoint(
                conn,
                "failCommand",
                {
                    errorCode: ErrorCodes.OperationFailed,
                    failCommands: [cmdName],
                    namespace: "test." + collName,
                },
                {times: 1},
            );

            assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.OperationFailed);
            assert.eq(coll.findOne({_id: 2}).a, 2, "Document should not be affected after failed attempt");

            let entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 0, "Expected no query stats after failed attempt");

            const retryResult = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(getCount(retryResult), 1);
            assertDocModified(coll, 2);

            entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after successful retry");
            assert.eq(entries[0].metrics.execCount, 1);

            fp.off();
        });
    });
}

/**
 * Scaffolds a one-way tokenization test suite for a write command against a specific topology.
 * Handles fixture lifecycle (before/after) and query stats store reset (beforeEach).
 *
 * @param {string} label - describe block label
 * @param {Function} setupFn - function() => {fixture, testDB}
 * @param {Function} teardownFn - function(fixture) => void
 * @param {string} collName - collection name
 * @param {Array|object} initialDocs - documents inserted before tests run
 * @param {Function} testBodyFn - function(ctxFn) that registers it() blocks;
 *   ctxFn() => {testDB, coll}, valid only at test execution time (after before())
 */
export function runTokenizationTestsForTopology(label, setupFn, teardownFn, {collName, initialDocs}, testBodyFn) {
    describe(label, function () {
        let fixture, testDB, coll;

        before(function () {
            const res = setupFn();
            fixture = res.fixture;
            testDB = res.testDB;
            coll = testDB[collName];
            coll.drop();
            assert.commandWorked(coll.insert(initialDocs));
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetQueryStatsStore(testDB.getMongo(), "1MB");
        });

        testBodyFn(() => ({testDB, coll}));
    });
}
