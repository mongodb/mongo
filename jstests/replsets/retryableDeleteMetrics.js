/**
 * Confirms that a retried delete records the retryable-write metrics.
 *   - transactions.retryableCommandsCount : total retryable delete commands received,
 *   - transactions.retriedCommandsCount / retriedStatementsCount : the retried subset.
 *
 * Uses a 1-node replica set so the counters are deterministic (no sharded background writes).
 * @tags: [requires_replication, multiversion_incompatible]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({name: "retryableDeleteMetrics", nodes: 1});
rst.startSet();
rst.initiate();

function getTransactionsStats() {
    return rst.getPrimary().adminCommand({serverStatus: 1}).transactions;
}

const primary = rst.getPrimary();
const coll = primary.getDB("test").getCollection("jstests_retryable_delete_metrics");
coll.drop();
assert.commandWorked(coll.insert({_id: 0, x: 1}));

const initialStatus = getTransactionsStats();
assert.hasFields(
    initialStatus,
    ["retryableCommandsCount", "retriedCommandsCount", "retriedStatementsCount"],
    "serverStatus.transactions missing the retryable-write metric fields",
);

jsTestLog.info("Initial status before test: ", initialStatus);

const cmd = {
    delete: coll.getName(),
    deletes: [{q: {_id: 0}, limit: 1}],
    lsid: {id: UUID()},
    txnNumber: NumberLong(1),
};

jsTestLog.info("Running command twice: ", cmd);

// First run deletes; the identical replay is served as a retry (stmtId defaults to 0).
assert.commandWorked(primary.getDB("test").runCommand(cmd));
assert.commandWorked(primary.getDB("test").runCommand(cmd));
assert.eq(0, coll.countDocuments({}), "delete replay must not delete again");

const newStatus = getTransactionsStats();
jsTestLog.info("New status after retrying write: ", newStatus);

// Two retryable delete commands were received; one of them was a retried statement.
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

// TODO(SERVER-134507): once the retry-delay latency histogram is surfaced in serverStatus, also
// assert that retriedWritesDelayMillis gained one sample in a low bucket.

rst.stopSet();
