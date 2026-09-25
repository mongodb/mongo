/**
 * Confirms that a retried bulkWrite records the retryable-write metrics, one
 * operation kind per test case so a failure isolates the offending op.
 *   - transactions.retryableCommandsCount : total retryable bulkWrite commands received,
 *   - transactions.retriedCommandsCount / retriedStatementsCount : the retried subset.
 *
 * Uses a 1-node replica set so the counters are deterministic (no sharded background writes).
 *
 * @tags: [requires_replication, multiversion_incompatible]
 */
import {before, beforeEach, describe, it, after} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("retryable bulkWrite metrics", function () {
    let rst;
    let primary;
    let db;
    let coll;

    before(function () {
        rst = new ReplSetTest({name: jsTestName(), nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        db = primary.getDB("test");
        coll = db.getCollection("jstests_retryable_bulkwrite_metrics");
    });

    beforeEach(function () {
        coll.drop();
    });

    after(function () {
        rst.stopSet();
    });

    function getTransactionsStats() {
        return primary.adminCommand({serverStatus: 1}).transactions;
    }

    it("records retryable insert metrics", function () {
        const lsid = {id: UUID()};
        const cmd = {
            bulkWrite: 1,
            ops: [{insert: 0, document: {_id: 0, x: 0}}],
            nsInfo: [{ns: coll.getFullName()}],
            lsid: lsid,
            txnNumber: NumberLong(1),
        };

        const initialStatus = getTransactionsStats();
        assert.hasFields(
            initialStatus,
            ["retryableCommandsCount", "retriedCommandsCount", "retriedStatementsCount"],
            "serverStatus.transactions missing the retryable-write metric fields",
        );
        jsTestLog.info("Initial status before insert: ", initialStatus);
        jsTestLog.info("Running insert command twice: ", cmd);

        // Initial attempt performs the insert.
        const result = assert.commandWorked(primary.getDB("admin").runCommand(cmd));
        assert.eq(1, result.nInserted, "first attempt should have inserted the document");

        // Remove the document in the middle with no lsid/txnNumber, so the session's
        // retryable-writes history is untouched.
        assert.commandWorked(
            db.runCommand({delete: coll.getName(), deletes: [{q: {_id: 0}, limit: 1}]}),
        );

        // The retry should receive the original reply despite the now missing document.
        const retryResult = assert.commandWorked(primary.getDB("admin").runCommand(cmd));
        assert.eq(
            0,
            coll.find({_id: 0}).itcount(),
            "retried insert must be served from history, not re-inserted",
        );
        assert.eq(
            result.nErrors,
            retryResult.nErrors,
            "replayed bulkWrite insert should return the same nErrors",
        );
        assert.eq(
            result.nInserted,
            retryResult.nInserted,
            "retried insert should replay the original reply",
        );

        const newStatus = getTransactionsStats();
        jsTestLog.info("New status after retrying insert: ", newStatus);

        // Two retryable bulkWrite insert commands were received.
        assert.eq(
            newStatus.retryableCommandsCount,
            initialStatus.retryableCommandsCount + 2,
            "expected retryableCommandsCount to increase by 2",
        );
        // Exactly one of them was a retried command (the replay), with one retried statement.
        assert.eq(
            newStatus.retriedCommandsCount,
            initialStatus.retriedCommandsCount + 1,
            "expected retriedCommandsCount to increase by 1",
        );
        assert.eq(
            newStatus.retriedStatementsCount,
            initialStatus.retriedStatementsCount + 1,
            "expected retriedStatementsCount to increase by 1",
        );

        // TODO(SERVER-134507): once the retry-delay latency histogram is surfaced in serverStatus,
        // also assert that retriedWritesDelayMillis gained one sample in a low bucket.
    });

    it("records retryable update metrics", function () {
        assert.commandWorked(coll.insert({_id: 1, x: 0}));

        const lsid = {id: UUID()};
        const cmd = {
            bulkWrite: 1,
            ops: [{update: 0, filter: {_id: 1}, updateMods: {$inc: {x: 1}}}],
            nsInfo: [{ns: coll.getFullName()}],
            lsid: lsid,
            txnNumber: NumberLong(2),
        };

        const initialStatus = getTransactionsStats();
        assert.hasFields(
            initialStatus,
            ["retryableCommandsCount", "retriedCommandsCount", "retriedStatementsCount"],
            "serverStatus.transactions missing the retryable-write metric fields",
        );
        jsTestLog.info("Initial status before update: ", initialStatus);
        jsTestLog.info("Running update command twice: ", cmd);

        // First run updates; the identical replay is served as a retry (stmtId auto-assigned).
        const result = assert.commandWorked(primary.getDB("admin").runCommand(cmd));
        const retryResult = assert.commandWorked(primary.getDB("admin").runCommand(cmd));
        assert.eq(
            result.nErrors,
            retryResult.nErrors,
            "replayed bulkWrite update should return the same nErrors",
        );
        assert.eq(
            {_id: 1, x: 1},
            coll.findOne({_id: 1}),
            "document should be correct after retried updates",
        );

        const newStatus = getTransactionsStats();
        jsTestLog.info("New status after retrying update: ", newStatus);

        assert.eq(
            newStatus.retryableCommandsCount,
            initialStatus.retryableCommandsCount + 2,
            "expected retryableCommandsCount to increase by 2",
        );
        assert.eq(
            newStatus.retriedCommandsCount,
            initialStatus.retriedCommandsCount + 1,
            "expected retriedCommandsCount to increase by 1",
        );
        assert.eq(
            newStatus.retriedStatementsCount,
            initialStatus.retriedStatementsCount + 1,
            "expected retriedStatementsCount to increase by 1",
        );

        // TODO(SERVER-134507): once the retry-delay latency histogram is surfaced in serverStatus,
        // also assert that retriedWritesDelayMillis gained one sample in a low bucket.
    });

    it("records retryable delete metrics", function () {
        assert.commandWorked(coll.insert({_id: 2, x: 0}));

        const lsid = {id: UUID()};
        const cmd = {
            bulkWrite: 1,
            ops: [{delete: 0, filter: {_id: 2}}],
            nsInfo: [{ns: coll.getFullName()}],
            lsid: lsid,
            txnNumber: NumberLong(3),
        };

        const initialStatus = getTransactionsStats();
        assert.hasFields(
            initialStatus,
            ["retryableCommandsCount", "retriedCommandsCount", "retriedStatementsCount"],
            "serverStatus.transactions missing the retryable-write metric fields",
        );
        jsTestLog.info("Initial status before delete: ", initialStatus);
        jsTestLog.info("Running delete command twice: ", cmd);

        // Initial attempt performs the delete.
        const result = assert.commandWorked(primary.getDB("admin").runCommand(cmd));
        assert.eq(1, result.nDeleted, "first attempt should have deleted the document");

        // Re-insert the document in the middle with no lsid/txnNumber, so the session's
        // retryable-writes history is untouched.
        assert.commandWorked(db.runCommand({insert: coll.getName(), documents: [{_id: 2, x: 0}]}));

        // The retry should receive the original reply without deleting the document again.
        const retryResult = assert.commandWorked(primary.getDB("admin").runCommand(cmd));
        assert.eq(
            1,
            coll.find({_id: 2}).itcount(),
            "retried delete must be served from history, not re-executed",
        );
        assert.eq(
            result.nErrors,
            retryResult.nErrors,
            "replayed bulkWrite delete should return the same nErrors",
        );
        assert.eq(
            result.nDeleted,
            retryResult.nDeleted,
            "retried delete should replay the original reply",
        );

        const newStatus = getTransactionsStats();
        jsTestLog.info("New status after retrying delete: ", newStatus);

        assert.eq(
            newStatus.retryableCommandsCount,
            initialStatus.retryableCommandsCount + 2,
            "expected retryableCommandsCount to increase by 2",
        );
        assert.eq(
            newStatus.retriedCommandsCount,
            initialStatus.retriedCommandsCount + 1,
            "expected retriedCommandsCount to increase by 1",
        );
        assert.eq(
            newStatus.retriedStatementsCount,
            initialStatus.retriedStatementsCount + 1,
            "expected retriedStatementsCount to increase by 1",
        );

        // TODO(SERVER-134507): once the retry-delay latency histogram is surfaced in serverStatus,
        // also assert that retriedWritesDelayMillis gained one sample in a low bucket.
    });
});
