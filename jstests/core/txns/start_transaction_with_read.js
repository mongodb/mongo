// Test transaction starting with read.
// @tags: [uses_transactions]
(function() {
"use strict";

const dbName = "test";
const collName = "start_transaction_with_read";

const testDB = db.getSiblingDB(dbName);
const coll = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}});

const sessionOptions = {
    causalConsistency: false
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

// Non-transactional write to give something to find.
const initialDoc = {
    _id: "pretransaction1",
    x: 0
};
assert.writeOK(sessionColl.insert(initialDoc, {writeConcern: {w: "majority"}}));

jsTest.log("Start a transaction with a read");

session.startTransaction();

let docs = sessionColl.find({}).toArray();
assert.sameMembers(docs, [initialDoc]);

jsTest.log("Insert two documents in a transaction");

// Insert a doc within the transaction.
assert.commandWorked(sessionColl.insert({_id: "insert-1"}));

// Read in the same transaction returns the doc.
docs = sessionColl.find({_id: "insert-1"}).toArray();
assert.sameMembers(docs, [{_id: "insert-1"}]);

// Insert a doc within a transaction.
assert.commandWorked(sessionColl.insert({_id: "insert-2"}));

assert.commandWorked(session.commitTransaction_forTesting());

// Read with default read concern sees the committed transaction.
assert.eq({_id: "insert-1"}, coll.findOne({_id: "insert-1"}));
assert.eq({_id: "insert-2"}, coll.findOne({_id: "insert-2"}));
assert.eq(initialDoc, coll.findOne(initialDoc));

session.endSession();
}());
