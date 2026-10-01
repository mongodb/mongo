/**
 * Confirms that retried retryable writes record the retryable-write metrics
 * (retryableCommandsCount, retriedCommandsCount, retriedStatementsCount) across
 * every command kind, with the common logic factored out.
 *   - findAndModify, update, insert, delete (regular collections),
 *   - bulkWrite insert/update/delete (run against the admin database).
 *
 * Time-series updates are tested in retryableTimeseriesUpdateMetrics.js where the appropriate
 * feature flags are enabled.
 *
 * Uses a 1-node replica set so the counters are deterministic (no sharded background writes).
 *
 * @tags: [requires_replication, multiversion_incompatible]
 */
import {before, beforeEach, describe, it, after} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("retryable writes metrics", function () {
    let rst;
    let primary;
    let db;
    let coll;

    let txnNumber;

    before(function () {
        rst = new ReplSetTest({name: jsTestName(), nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        db = primary.getDB("test");
        coll = db.getCollection(jsTestName() + "coll");
        txnNumber = 0;
    });

    beforeEach(function () {
        coll.drop();
        txnNumber++;
    });

    after(function () {
        rst.stopSet();
    });

    function getTransactionsStats() {
        return primary.adminCommand({serverStatus: 1}).transactions;
    }

    /**
     * Runs `cmd` twice (first performs the write, second is the replay) and asserts
     * the retryable-write metrics moved by the expected amounts. `adminDb` selects
     * whether the command must run against the admin database (bulkWrite) vs the
     * test database (everything else). `betweenRuns`, if given, runs after the first attempt
     * and before the replay, e.g. to modify the document without a session so that `checkDoc`
     * can prove the replay was served from history rather than re-executed.
     */
    function testRetryableWritesMetrics(name, cmd, checkResult, checkDoc, adminDb, betweenRuns) {
        const retryableCmd = cmd;
        retryableCmd.lsid = {id: UUID()};
        retryableCmd.txnNumber = NumberLong(txnNumber);

        const initialStatus = getTransactionsStats();
        assert.hasFields(
            initialStatus,
            ["retryableCommandsCount", "retriedCommandsCount", "retriedStatementsCount"],
            "serverStatus.transactions missing the retryable-write metric fields",
        );
        assert(
            initialStatus.hasOwnProperty("retriedWritesDelayMillis"),
            "serverStatus.transactions missing retriedWritesDelayMillis before " + name,
        );

        jsTestLog.info("Initial status before " + name + ": ", initialStatus);
        jsTestLog.info("Running " + name + " command twice: ", retryableCmd);

        const runDb = adminDb ? primary.getDB("admin") : db;
        const result = assert.commandWorked(runDb.runCommand(retryableCmd));
        if (betweenRuns) {
            betweenRuns();
        }
        const retryResult = assert.commandWorked(runDb.runCommand(retryableCmd));
        checkResult(result, retryResult);

        const newStatus = getTransactionsStats();
        jsTestLog.info("New status after retrying " + name + ": ", newStatus);

        // Two retryable commands were received; one of them was a retried statement.
        assert.eq(
            newStatus.retryableCommandsCount,
            initialStatus.retryableCommandsCount + 2,
            "expected retryableCommandsCount to increase by 2 (" + name + ")",
        );
        assert.eq(
            newStatus.retriedCommandsCount,
            initialStatus.retriedCommandsCount + 1,
            "expected retriedCommandsCount to increase by 1 (" + name + ")",
        );
        assert.eq(
            newStatus.retriedStatementsCount,
            initialStatus.retriedStatementsCount + 1,
            "expected retriedStatementsCount to increase by 1 (" + name + ")",
        );

        checkDoc();

        // Assert the retry-delay histogram gained one sample.
        const initialHist = initialStatus.retriedWritesDelayMillis || [];
        const initialTotalInHist = initialHist.reduce((a, b) => a + b.count, 0);
        const hist = newStatus.retriedWritesDelayMillis || [];
        const afterTotalInHist = hist.reduce((a, b) => a + b.count, 0);
        assert.eq(
            afterTotalInHist,
            initialTotalInHist + 1,
            "expected retriedWritesDelayMillis to gain one sample",
        );
    }

    it("records retryable findAndModify metrics", function () {
        assert.commandWorked(coll.insert({_id: 0, x: 0}));
        const cmd = {
            findAndModify: coll.getName(),
            query: {_id: 0},
            update: {$inc: {x: 1}},
            new: true,
        };
        testRetryableWritesMetrics(
            "findAndModify",
            cmd,
            function (result, retryResult) {
                assert.eq(
                    result.value,
                    retryResult.value,
                    "replayed findAndModify should return the same value",
                );
            },
            function () {
                assert.eq(
                    {_id: 0, x: 1},
                    coll.findOne({_id: 0}),
                    "findAndModify replay must not re-apply",
                );
            },
        );
    });

    it("records retryable update metrics", function () {
        assert.commandWorked(coll.insert({_id: 0, x: 0}));
        const incX = {$inc: {x: 1}};
        const cmd = {
            update: coll.getName(),
            updates: [{q: {_id: 0}, u: incX}],
        };
        testRetryableWritesMetrics(
            "update",
            cmd,
            function (result, retryResult) {
                assert.eq(result.n, retryResult.n, "replayed update should return the same n");
            },
            function () {
                assert.eq(
                    {_id: 0, x: 1},
                    coll.findOne({_id: 0}),
                    "update replay must not re-apply",
                );
            },
        );
    });

    it("records retryable insert metrics", function () {
        const cmd = {
            insert: coll.getName(),
            documents: [{_id: 0, x: 0}],
        };
        testRetryableWritesMetrics(
            "insert",
            cmd,
            function (result, retryResult) {
                assert.eq(result.n, retryResult.n, "replayed insert should return the same n");
            },
            function () {
                assert.eq(1, coll.countDocuments({}), "insert replay must not re-insert");
            },
        );
    });

    it("records retryable delete metrics", function () {
        assert.commandWorked(coll.insert({_id: 2, x: 0}));
        const cmd = {
            delete: coll.getName(),
            deletes: [{q: {_id: 2}, limit: 1}],
        };
        testRetryableWritesMetrics(
            "delete",
            cmd,
            function (result, retryResult) {
                assert.eq(result.n, retryResult.n, "replayed delete should return the same n");
            },
            function () {
                assert.eq(null, coll.findOne({_id: 2}), "delete replay must not delete again");
            },
        );
    });

    it("records retryable bulkWrite insert metrics", function () {
        const cmd = {
            bulkWrite: 1,
            ops: [{insert: 0, document: {_id: 0, x: 0}}],
            nsInfo: [{ns: coll.getFullName()}],
        };
        testRetryableWritesMetrics(
            "bulkWrite insert",
            cmd,
            function (result, retryResult) {
                assert.eq(
                    result.nErrors,
                    retryResult.nErrors,
                    "replayed bulkWrite insert should return the same nErrors",
                );
                assert.eq(1, result.nInserted, "first attempt should have inserted the document");
                assert.eq(
                    result.nInserted,
                    retryResult.nInserted,
                    "replayed bulkWrite insert should return the same nInserted",
                );
            },
            function () {
                assert.eq(
                    0,
                    coll.find({_id: 0}).itcount(),
                    "retried bulkWrite insert must be served from history, not re-inserted",
                );
            },
            true,
            function () {
                // Remove the document with no lsid/txnNumber, so the session's retryable-writes
                // history is untouched.
                assert.commandWorked(
                    db.runCommand({delete: coll.getName(), deletes: [{q: {_id: 0}, limit: 1}]}),
                );
            },
        );
    });

    it("records retryable bulkWrite update metrics", function () {
        assert.commandWorked(coll.insert({_id: 1, x: 0}));
        const incX = {$inc: {x: 1}};
        const cmd = {
            bulkWrite: 1,
            ops: [{update: 0, filter: {_id: 1}, updateMods: incX}],
            nsInfo: [{ns: coll.getFullName()}],
        };
        testRetryableWritesMetrics(
            "bulkWrite update",
            cmd,
            function (result, retryResult) {
                assert.eq(
                    result.nErrors,
                    retryResult.nErrors,
                    "replayed bulkWrite update should return the same nErrors",
                );
            },
            function () {
                assert.eq(
                    {_id: 1, x: 1},
                    coll.findOne({_id: 1}),
                    "bulkWrite update replay must not re-apply",
                );
            },
            true,
        );
    });

    it("records retryable bulkWrite delete metrics", function () {
        assert.commandWorked(coll.insert({_id: 2, x: 0}));
        const cmd = {
            bulkWrite: 1,
            ops: [{delete: 0, filter: {_id: 2}}],
            nsInfo: [{ns: coll.getFullName()}],
        };
        testRetryableWritesMetrics(
            "bulkWrite delete",
            cmd,
            function (result, retryResult) {
                assert.eq(
                    result.nErrors,
                    retryResult.nErrors,
                    "replayed bulkWrite delete should return the same nErrors",
                );
                assert.eq(1, result.nDeleted, "first attempt should have deleted the document");
                assert.eq(
                    result.nDeleted,
                    retryResult.nDeleted,
                    "replayed bulkWrite delete should return the same nDeleted",
                );
            },
            function () {
                assert.eq(
                    1,
                    coll.find({_id: 2}).itcount(),
                    "retried bulkWrite delete must be served from history, not re-executed",
                );
            },
            true,
            function () {
                // Re-insert the document with no lsid/txnNumber, so the session's retryable-writes
                // history is untouched.
                assert.commandWorked(
                    db.runCommand({insert: coll.getName(), documents: [{_id: 2, x: 0}]}),
                );
            },
        );
    });
});
