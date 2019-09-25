/**
 * Test that killing of sessions leaves prepared transactions intact.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "kill_sessions_with_prepared_transaction";
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);

// Create a collection.
assert.commandWorked(primaryDB.createCollection(collName));

const session = primary.startSession();
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

const session2 = primary.startSession();
const session2Db = session2.getDatabase(dbName);
const session2Coll = session2Db[collName];

const session3 = primary.startSession();
const session3Db = session3.getDatabase(dbName);
const session3Coll = session3Db[collName];

// Produce a currentOp filter for a prepared transaction on a given session.
function preparedTxnOpFilter(session) {
    return {
        "lsid.id": session.getSessionId().id,
        "transaction.timePreparedMicros": {$exists: true}
    };
}

//
// Test killing a single session with a prepared transaction.
//
jsTestLog("Starting and preparing a transaction.");

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 0}));
const commitTs = PrepareHelpers.prepareTransaction(session);

jsTestLog("Killing the session of the prepared transaction.");
assert.commandWorked(primaryDB.runCommand({killSessions: [session.getSessionId()]}));

// Make sure the prepared transaction is still intact.
assert.eq(primaryDB.currentOp(preparedTxnOpFilter(session)).inprog.length, 1);

// Commit the transaction.
assert.commandWorked(PrepareHelpers.commitTransaction(session, commitTs));

// Make sure the effects of the transaction are visible.
assert.sameMembers([{_id: 0}], sessionColl.find().toArray());
assert.commandWorked(sessionColl.remove({}, {writeConcern: {w: "majority"}}));

//
// Test killing multiple sessions, some with prepared transactions and some without.
//
jsTestLog("Starting and preparing two transactions on different sessions.");

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));
const commitTs1 = PrepareHelpers.prepareTransaction(session);

session2.startTransaction();
assert.commandWorked(session2Coll.insert({_id: 2}));
const commitTs2 = PrepareHelpers.prepareTransaction(session2);

jsTestLog("Starting a transaction that will not be prepared.");

session3.startTransaction();
assert.commandWorked(session3Coll.insert({_id: 3}));

jsTestLog("Killing all sessions.");
assert.commandWorked(primaryDB.runCommand({killAllSessions: []}));

// Make sure the prepared transactions are still intact.
assert.eq(primaryDB.currentOp(preparedTxnOpFilter(session)).inprog.length, 1);
assert.eq(primaryDB.currentOp(preparedTxnOpFilter(session2)).inprog.length, 1);

// The unprepared transaction should have been aborted when its session was killed.
assert.commandFailedWithCode(session3Db.adminCommand({commitTransaction: 1}),
                             ErrorCodes.NoSuchTransaction);

// Commit each transaction.
assert.commandWorked(PrepareHelpers.commitTransaction(session, commitTs1));
assert.commandWorked(PrepareHelpers.commitTransaction(session2, commitTs2));

// Make sure the effects of the transactions are visible.
assert.sameMembers([{_id: 1}, {_id: 2}], sessionColl.find().toArray());

rst.stopSet();
}());
