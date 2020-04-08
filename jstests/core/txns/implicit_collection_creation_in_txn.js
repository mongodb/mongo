// Tests that it is allowed to implicitly create a collection using insert or upsert in a
// multi-document transaction.
// @tags: [uses_transactions, requires_fcv_44]
(function() {
"use strict";

const dbName = "test";
const collName = "implicit_collection_creation_in_txn";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

const session = db.getMongo().startSession();
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

jsTest.log("Implicitly create a collection in a transaction using insert.");

// Insert succeeds when the collection exists.
assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

session.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.insert({_id: "doc"}));
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq({_id: "doc"}, testColl.findOne({_id: "doc"}));

// Insert succeeds when the collection does not exist.
assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

session.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.insert({_id: "doc"}));
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq({_id: "doc"}, testColl.findOne({_id: "doc"}));

assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

jsTest.log("Implicitly create a collection in a transaction using update.");

// Update with upsert=true succeeds when the collection exists.
assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

session.startTransaction({writeConcern: {w: "majority"}});
sessionColl.update({_id: "doc"}, {$set: {updated: true}}, {upsert: true});
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

// Update with upsert=true succeeds when the collection does not exist.
assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

session.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.update({_id: "doc"}, {$set: {updated: true}}, {upsert: true}));
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

// Update with upsert=false succeeds when the collection does not exist.
assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));
session.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.update({_id: "doc"}, {$set: {updated: true}}, {upsert: false}));
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(null, testColl.findOne({_id: "doc"}));

jsTest.log("Implicitly create a collection in a transaction using findAndModify.");

// findAndModify with upsert=true succeeds when the collection exists.
assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

session.startTransaction({writeConcern: {w: "majority"}});
let res = sessionDb.runCommand(
    {findAndModify: collName, query: {_id: "doc"}, update: {$set: {updated: true}}, upsert: true});
assert.commandWorked(res);
assert.eq(null, res.value);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

// findAndModify with upsert=true succeeds when the collection does not exist.
assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

session.startTransaction({writeConcern: {w: "majority"}});
res = sessionDb.runCommand(
    {findAndModify: collName, query: {_id: "doc"}, update: {$set: {updated: true}}, upsert: true});
assert.commandWorked(res);
assert.eq(null, res.value);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

// findAndModify with upsert=false succeeds when the collection does not exist.
assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));
session.startTransaction({writeConcern: {w: "majority"}});
res = sessionDb.runCommand(
    {findAndModify: collName, query: {_id: "doc"}, update: {$set: {updated: true}}, upsert: false});
assert.commandWorked(res);
assert.eq(null, res.value);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(null, testColl.findOne({_id: "doc"}));

session.endSession();
}());
