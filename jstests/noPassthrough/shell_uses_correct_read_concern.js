/**
 * Tests that reads through the shell uses the correct read concern.
 * @tags: [requires_replication, uses_transactions, requires_majority_read_concern]
 */
(function() {
"use strict";
const rst = new ReplSetTest({nodes: 2, nodeOptions: {enableMajorityReadConcern: ""}});
rst.startSet();
rst.initiate();

const collName = "shell_uses_transaction_read_concern";
const primary = rst.getPrimary();
const db = primary.getDB("test");
var coll = db.getCollection(collName);
const testDoc = {
    "test": "doc",
    "_id": 0
};
assert.commandWorked(coll.insertOne(testDoc));
rst.awaitReplication();

const getMajorityRCCount = () =>
    db.runCommand({serverStatus: 1}).readConcernCounters.nonTransactionOps.majority;
const getSnapshotRCCount = () =>
    db.runCommand({serverStatus: 1}).readConcernCounters.transactionOps.snapshot.withoutClusterTime;

// Command-level
assert.eq(coll.runCommand({"find": coll.getName(), readConcern: {level: "majority"}})
              .cursor.firstBatch.length,
          1);
assert.eq(getMajorityRCCount(), 1);

const session = primary.startSession({readConcern: {level: "majority"}});
coll = session.getDatabase("test").getCollection(collName);

// Session-level
assert.eq(coll.find({"_id": 0}).itcount(), 1);
assert.eq(coll.runCommand({"find": coll.getName()}).cursor.firstBatch.length, 1);
assert.eq(getMajorityRCCount(), 3);

// Check that the session read concern doesn't break explain.
assert.commandWorked(coll.runCommand(
    {explain: {count: collName, query: {"_id": 0}, readConcern: {level: "available"}}}));
assert.commandWorked(coll.runCommand({explain: {count: collName, query: {"_id": 0}}}));

// Transaction-level
session.startTransaction({readConcern: {level: "snapshot"}});
assert.eq(coll.runCommand({"find": coll.getName()}).cursor.firstBatch.length, 1);
assert.eq(coll.runCommand({"find": coll.getName()}).cursor.firstBatch.length, 1);
assert.eq(coll.find({"_id": 0}).itcount(), 1);
assert.docEq(coll.findOne({"_id": 0}), testDoc);

assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(getSnapshotRCCount(), 4);

rst.stopSet();
})();
