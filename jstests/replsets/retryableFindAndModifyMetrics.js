/**
 * Confirms that a retried findAndModify records the retryable-write metrics.
 *   - transactions.retryableCommandsCount : total retryable findAndModify commands received,
 *   - transactions.retriedCommandsCount / retriedStatementsCount : the retried subset.
 *
 * Uses a 1-node replica set because retryable writes require a replica set.
 *
 * @tags: [requires_replication, multiversion_incompatible]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({name: "retryableFindAndModifyMetrics", nodes: 1});
rst.startSet();
rst.initiate();

function getTransactionsStats() {
    return rst.getPrimary().adminCommand({serverStatus: 1}).transactions;
}

const primary = rst.getPrimary();
const coll = primary.getDB("test").getCollection("jstests_retryable_find_and_modify_metrics");
coll.drop();
assert.commandWorked(coll.insert({_id: 0, x: 0}));

const initialStatus = getTransactionsStats();
assert.hasFields(
    initialStatus,
    ["retryableCommandsCount", "retriedCommandsCount", "retriedStatementsCount"],
    "serverStatus.transactions missing the retryable-write metric fields",
);

jsTestLog.info("Initial status before test: ", initialStatus);

// Use a unique lsid per run. A fixed lsid would collide with a prior run's session when burn_in
// reuses the same fixture, causing this run's first findAndModify to be mistaken for a retry (and
// not applied).
const lsid = UUID();

const cmd = {
    findAndModify: coll.getName(),
    query: {_id: 0},
    update: {$inc: {x: 1}},
    new: true,
    lsid: {id: lsid},
    txnNumber: NumberLong(1),
};

jsTestLog.info("Running command twice: ", cmd);

// First run writes; the identical replay is served as a retry (stmtId defaults to 0).
const result = assert.commandWorked(primary.getDB("test").runCommand(cmd));
const retryResult = assert.commandWorked(primary.getDB("test").runCommand(cmd));
assert.eq(result.value, retryResult.value, "replayed findAndModify should return the same value");
assert.eq(
    {_id: 0, x: 1},
    coll.findOne({_id: 0}),
    "findAndModify replay must not re-apply the update",
);

const newStatus = getTransactionsStats();
jsTestLog.info("New status after retrying write: ", newStatus);

// Total retryable commands: two retryable findAndModify commands were received.
assert.eq(
    newStatus.retryableCommandsCount,
    initialStatus.retryableCommandsCount + 2,
    "expected retryableCommandsCount to increase by 2",
);
// Total actually retried: exactly one of them was a retried command with one retried statement.
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

// TODO(SERVER-134507): Test that the retry delay metrics are also present here.

rst.stopSet();
