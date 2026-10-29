/**
 * Verifies that a WT_ROLLBACK at commit_transaction due to WT_STEP_DOWN is retried for CRUD operations.
 * Uses the WTStepDownRollbackAtCommit failpoint to simulate the error.
 *
 * @tags: [requires_wiredtiger]
 */
const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.getCollection("wt_rollback_commit");
coll.drop();

function getWriteConflicts() {
    return db.serverStatus().metrics.operation.writeConflicts;
}

// Insert: fail once on commit, then succeed on retry.
let wcBefore = getWriteConflicts();
assert.commandWorked(
    conn.adminCommand({configureFailPoint: "WTStepDownRollbackAtCommit", mode: {times: 1}}),
);
assert.commandWorked(coll.insert({_id: 1, x: "inserted"}));
assert.gt(getWriteConflicts(), wcBefore, "expected writeConflicts counter to increase on insert");
assert.eq(coll.findOne({_id: 1}).x, "inserted");

// Update: fail once on commit, then succeed on retry.
wcBefore = getWriteConflicts();
assert.commandWorked(
    conn.adminCommand({configureFailPoint: "WTStepDownRollbackAtCommit", mode: {times: 1}}),
);
assert.commandWorked(coll.update({_id: 1}, {$set: {x: "updated"}}));
assert.gt(getWriteConflicts(), wcBefore, "expected writeConflicts counter to increase on update");
assert.eq(coll.findOne({_id: 1}).x, "updated");

// Delete: fail once on commit, then succeed on retry.
wcBefore = getWriteConflicts();
assert.commandWorked(
    conn.adminCommand({configureFailPoint: "WTStepDownRollbackAtCommit", mode: {times: 1}}),
);
assert.commandWorked(coll.remove({_id: 1}));
assert.gt(getWriteConflicts(), wcBefore, "expected writeConflicts counter to increase on delete");
assert.eq(coll.findOne({_id: 1}), null);

MongoRunner.stopMongod(conn);
