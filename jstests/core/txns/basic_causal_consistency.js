// Test that the shell helper supports causal consistency.
// @tags: [uses_transactions, uses_snapshot_read_concern]
(function() {
"use strict";

const dbName = "test";
const collName = "basic_causal_consistency";
const testDB = db.getSiblingDB(dbName);

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: true
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb.getCollection(collName);

session.startTransaction({readConcern: {level: "snapshot"}});

// Performing a read first should work when snapshot readConcern is specified.
assert.docEq(null, sessionColl.findOne({_id: "insert-1"}));

assert.commandWorked(sessionColl.insert({_id: "insert-1"}));

assert.docEq(null, sessionColl.findOne({_id: "insert-2"}));

assert.docEq({_id: "insert-1"}, sessionColl.findOne({_id: "insert-1"}));

assert.commandWorked(session.commitTransaction_forTesting());

session.endSession();
}());
