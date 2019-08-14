/**
 * Test that transaction oplog entries are not accepted by the 'applyOps' command.
 *
 * In 4.2, there are no MongoDB backup services that rely on applyOps based mechanisms, and any
 * other external tools that use applyOps should be converting transactional oplog entries to a
 * non-transactional format before running them through applyOps.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, exclude_from_large_txns]
 */
(function() {
"use strict";

load('jstests/core/txns/libs/prepare_helpers.js');

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({
    name: collName,
    nodes: 1,
    // Make it easy to generate multiple oplog entries per transaction.
    nodeOptions: {setParameter: {maxNumberOfTransactionOperationsInSingleOplogEntry: 1}}
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

// Initiate a session on the primary.
const sessionOptions = {
    causalConsistency: false
};
const primarySession = primary.getDB(dbName).getMongo().startSession(sessionOptions);
const primarySessionDb = primarySession.getDatabase(dbName);
const primarySessionColl = primarySessionDb[collName];

// Create a collection.
assert.commandWorked(primarySessionColl.insert({}));

//
// Run transactions of different varieties and record the oplog entries they generate, so that we
// can later try to apply them via the 'applyOps' command.
//

let oplog = primary.getDB("local")["oplog.rs"];
let sessionId = primarySession.getSessionId().id;

// Run an unprepared transaction that commits.
primarySession.startTransaction();
assert.commandWorked(primarySessionColl.insert({x: 1}));
assert.commandWorked(primarySessionColl.insert({x: 2}));
assert.commandWorked(primarySession.commitTransaction_forTesting());

let txnNum = primarySession.getTxnNumber_forTesting();
let unpreparedTxnOps = oplog.find({"lsid.id": sessionId, txnNumber: txnNum}).toArray();
assert.eq(unpreparedTxnOps.length, 2, "unexpected op count: " + tojson(unpreparedTxnOps));

// Run a prepared transaction that commits.
primarySession.startTransaction();
assert.commandWorked(primarySessionColl.insert({x: 1}));
assert.commandWorked(primarySessionColl.insert({x: 2}));
let prepareTs = PrepareHelpers.prepareTransaction(primarySession);
PrepareHelpers.commitTransaction(primarySession, prepareTs);

txnNum = primarySession.getTxnNumber_forTesting();
let preparedAndCommittedTxnOps = oplog.find({"lsid.id": sessionId, txnNumber: txnNum}).toArray();
assert.eq(preparedAndCommittedTxnOps.length,
          3,
          "unexpected op count: " + tojson(preparedAndCommittedTxnOps));

// Run a prepared transaction that aborts.
primarySession.startTransaction();
assert.commandWorked(primarySessionColl.insert({x: 1}));
assert.commandWorked(primarySessionColl.insert({x: 2}));
PrepareHelpers.prepareTransaction(primarySession);
assert.commandWorked(primarySession.abortTransaction_forTesting());

txnNum = primarySession.getTxnNumber_forTesting();
let preparedAndAbortedTxnOps = oplog.find({"lsid.id": sessionId, txnNumber: txnNum}).toArray();
assert.eq(
    preparedAndAbortedTxnOps.length, 3, "unexpected op count: " + tojson(preparedAndAbortedTxnOps));

// Clear out any documents that may have been created in the collection.
assert.commandWorked(primarySessionColl.remove({}));

//
// Now we test running the various transaction ops we captured through the 'applyOps' command.
//

let op = unpreparedTxnOps[0];  // in-progress op.
jsTestLog("Testing in-progress transaction op: " + tojson(op));
assert.commandFailedWithCode(primarySessionDb.adminCommand({applyOps: [op]}), 31056);

op = unpreparedTxnOps[1];  // implicit commit op.
jsTestLog("Testing unprepared implicit commit transaction op: " + tojson(op));
assert.commandFailedWithCode(primarySessionDb.adminCommand({applyOps: [op]}), 31240);

op = preparedAndCommittedTxnOps[1];  // implicit prepare op.
jsTestLog("Testing implicit prepare transaction op: " + tojson(op));
assert.commandFailedWithCode(primarySessionDb.adminCommand({applyOps: [op]}), 51145);

op = preparedAndCommittedTxnOps[2];  // prepared commit op.
jsTestLog("Testing prepared commit transaction op: " + tojson(op));
assert.commandFailedWithCode(primarySessionDb.adminCommand({applyOps: [op]}), 50987);

op = preparedAndAbortedTxnOps[2];  // prepared abort op.
jsTestLog("Testing prepared abort transaction op: " + tojson(op));
assert.commandFailedWithCode(primarySessionDb.adminCommand({applyOps: [op]}), 50972);

rst.stopSet();
}());
