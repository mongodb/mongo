// Test basic transaction write ops, reads, and commit/abort using the shell helper.
// @tags: [uses_transactions, uses_snapshot_read_concern]
(function() {
"use strict";

// TODO (SERVER-39704): Remove the following load after SERVER-397074 is completed
// For withTxnAndAutoRetryOnMongos.
load('jstests/libs/auto_retry_transaction_in_sharding.js');

const dbName = "test";
const collName = "multi_transaction_test_using_api";
const testDB = db.getSiblingDB(dbName);

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb.getCollection(collName);

//
// Test that calling abortTransaction as the first statement in a transaction is allowed and
// modifies the state accordingly.
//
jsTestLog("Call abortTransaction as the first statement in a transaction");
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

// Successfully call abortTransaction.
assert.commandWorked(session.abortTransaction_forTesting());

//
// Test that calling commitTransaction as the first statement in a transaction is allowed and
// modifies the state accordingly.
//
jsTestLog("Call commitTransaction as the first statement in a transaction");
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

// Successfully call commitTransaction.
assert.commandWorked(session.commitTransaction_forTesting());

jsTestLog("Run CRUD ops, read ops, and commit transaction.");

// TODO (SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(session, () => {
    // Performing a read first should work when snapshot readConcern is specified.
    assert.docEq(null, sessionColl.findOne({_id: "insert-1"}));

    assert.commandWorked(sessionColl.insert({_id: "insert-1", a: 0}));

    assert.commandWorked(sessionColl.insert({_id: "insert-2", a: 0}));

    assert.commandWorked(sessionColl.insert({_id: "insert-3", a: 0}));

    assert.commandWorked(sessionColl.update({_id: "insert-1"}, {$inc: {a: 1}}));

    assert.commandWorked(sessionColl.deleteOne({_id: "insert-2"}));

    sessionColl.findAndModify({query: {_id: "insert-3"}, update: {$set: {a: 2}}});

    // Try to find a document within a transaction.
    let cursor = sessionColl.find({_id: "insert-1"});
    assert.docEq({_id: "insert-1", a: 1}, cursor.next());
    assert(!cursor.hasNext());

    // Try to find a document using findOne within a transaction
    assert.eq({_id: "insert-1", a: 1}, sessionColl.findOne({_id: "insert-1"}));

    // Find a document with the aggregation shell helper within a transaction.
    cursor = sessionColl.aggregate({$match: {_id: "insert-1"}});
    assert.docEq({_id: "insert-1", a: 1}, cursor.next());
    assert(!cursor.hasNext());
}, {readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

// Make sure the correct documents exist after committing the transaciton.
assert.eq({_id: "insert-1", a: 1}, sessionColl.findOne({_id: "insert-1"}));
assert.eq({_id: "insert-3", a: 2}, sessionColl.findOne({_id: "insert-3"}));
assert.eq(null, sessionColl.findOne({_id: "insert-2"}));

jsTestLog("Insert a doc and abort transaction.");
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

assert.commandWorked(sessionColl.insert({_id: "insert-4", a: 0}));

assert.commandWorked(session.abortTransaction_forTesting());

// Verify that we cannot see the document we tried to insert.
assert.eq(null, sessionColl.findOne({_id: "insert-4"}));

jsTestLog("Bulk insert and update operations within transaction.");

session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
let bulk = sessionColl.initializeUnorderedBulkOp();
bulk.insert({_id: "bulk-1"});
bulk.insert({_id: "bulk-2"});
bulk.find({_id: "bulk-1"}).updateOne({$set: {status: "bulk"}});
bulk.find({_id: "bulk-2"}).updateOne({$set: {status: "bulk"}});
assert.commandWorked(bulk.execute());
assert.commandWorked(session.commitTransaction_forTesting());

assert.eq({_id: "bulk-1", status: "bulk"}, sessionColl.findOne({_id: "bulk-1"}));
assert.eq({_id: "bulk-2", status: "bulk"}, sessionColl.findOne({_id: "bulk-2"}));

jsTestLog("Bulk delete operations within transaction.");

session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
bulk = sessionColl.initializeUnorderedBulkOp();
bulk.find({_id: "bulk-1"}).removeOne();
bulk.find({_id: "bulk-2"}).removeOne();
assert.commandWorked(bulk.execute());
assert.commandWorked(session.commitTransaction_forTesting());

assert.eq(null, sessionColl.findOne({_id: "bulk-1"}));
assert.eq(null, sessionColl.findOne({_id: "bulk-2"}));

session.endSession();
}());
