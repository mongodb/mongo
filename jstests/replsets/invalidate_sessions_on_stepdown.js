/**
 * Tests that a txnNumber for a transaction that was aborted in-memory can be reused by a new
 * primary. Mongos does this as an optimization.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

const rst = new ReplSetTest({name: "invalidate_sessions_on_stepdown", nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = "test";
const collName = "coll";
const node0 = rst.getPrimary();
const node1 = rst.getSecondary();
const node0Session = node0.startSession();
const sessionId = node0Session.getSessionId();
const node0SessionDB = node0Session.getDatabase(dbName);

assert.commandWorked(node0SessionDB.coll.insert({a: 1}, {writeConcern: {"w": "majority"}}));

jsTestLog("Run a transaction with txnNumber 0 on the primary.");
assert.commandWorked(node0SessionDB.runCommand({
    insert: collName,
    documents: [{b: 1}],
    txnNumber: NumberLong(0),
    startTransaction: true,
    autocommit: false
}));

jsTestLog("Step up the secondary. The primary will abort the transaction when it steps down.");
assert.commandWorked(node1.adminCommand({replSetStepUp: 1}));
assert.eq(node1, rst.getPrimary());

const node1DB = node1.getDB(dbName);

jsTestLog("Run a transaction with txnNumber 0 and the same session ID on the new primary.");
assert.commandWorked(node1DB.runCommand({
    insert: collName,
    documents: [{c: 1}],
    lsid: sessionId,
    txnNumber: NumberLong(0),
    startTransaction: true,
    autocommit: false
}));
let res = assert.commandWorked(node1DB.adminCommand({
    prepareTransaction: 1,
    lsid: sessionId,
    txnNumber: NumberLong(0),
    autocommit: false,
    writeConcern: {w: "majority"}
}));
assert.commandWorked(node1DB.adminCommand({
    commitTransaction: 1,
    commitTimestamp: res.prepareTimestamp,
    lsid: sessionId,
    txnNumber: NumberLong(0),
    autocommit: false,
    writeConcern: {w: "majority"}
}));
assert.eq(2, node0SessionDB.coll.find().itcount());
assert.eq(0, node0SessionDB.coll.find({b: 1}).itcount());
assert.eq(1, node0SessionDB.coll.find({c: 1}).itcount());

rst.stopSet();
})();
