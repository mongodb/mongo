// Test transactions including find-and-modify
// @tags: [assumes_unsharded_collection, uses_transactions]
(function() {
"use strict";

const dbName = "test";
const collName = "find_and_modify_in_transaction";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

jsTest.log("Prepopulate the collection.");
assert.writeOK(testColl.insert([{_id: 0, a: 0}, {_id: 1, a: 1}, {_id: 2, a: 2}],
                               {writeConcern: {w: "majority"}}));

/***********************************************************************************************
 * Do a non-matching find-and-modify with remove.
 **********************************************************************************************/

jsTest.log("Do a non-matching find-and-modify with remove.");
session.startTransaction({writeConcern: {w: "majority"}});

// Do a findAndModify that affects no documents.
let res = sessionColl.findAndModify({query: {a: 99}, remove: true});
assert.eq(null, res);
let docs = sessionColl.find({}).toArray();
assert.sameMembers(docs, [{_id: 0, a: 0}, {_id: 1, a: 1}, {_id: 2, a: 2}]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());

/***********************************************************************************************
 * Do a non-matching find-and-modify with update.
 **********************************************************************************************/

jsTest.log("Do a non-matching find-and-modify with update.");

session.startTransaction({writeConcern: {w: "majority"}});

res = sessionColl.findAndModify({query: {a: 99}, update: {$inc: {a: 100}}});
assert.eq(null, res);
docs = sessionColl.find({}).toArray();
assert.sameMembers(docs, [{_id: 0, a: 0}, {_id: 1, a: 1}, {_id: 2, a: 2}]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());

/***********************************************************************************************
 * Do a matching find-and-modify with remove.
 **********************************************************************************************/

jsTest.log("Do a matching find-and-modify with remove.");

session.startTransaction({writeConcern: {w: "majority"}});

res = sessionColl.findAndModify({query: {a: 0}, remove: true});
assert.eq({_id: 0, a: 0}, res);
docs = sessionColl.find({}).toArray();
assert.sameMembers(docs, [{_id: 1, a: 1}, {_id: 2, a: 2}]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());

/***********************************************************************************************
 * Do a matching find-and-modify with update, requesting the old doc.
 **********************************************************************************************/

jsTest.log("Do a matching find-and-modify with update, requesting the old doc.");
session.startTransaction({writeConcern: {w: "majority"}});

res = sessionColl.findAndModify({query: {a: 1}, update: {$inc: {a: 100}}});
assert.eq({_id: 1, a: 1}, res);
docs = sessionColl.find({}).toArray();
assert.sameMembers(docs, [{_id: 1, a: 101}, {_id: 2, a: 2}]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());

/***********************************************************************************************
 * Do a matching find-and-modify with update, requesting the new doc.
 **********************************************************************************************/

jsTest.log("Do a matching find-and-modify with update, requesting the new doc.");
session.startTransaction({writeConcern: {w: "majority"}});

res = sessionColl.findAndModify({query: {a: 2}, update: {$inc: {a: 100}}, new: true});
assert.eq({_id: 2, a: 102}, res);
docs = sessionColl.find({}).toArray();
assert.sameMembers(docs, [{_id: 1, a: 101}, {_id: 2, a: 102}]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());

/***********************************************************************************************
 * Do a matching find-and-modify with upsert, requesting the new doc.
 **********************************************************************************************/

jsTest.log("Do a matching find-and-modify with upsert, requesting the new doc.");
session.startTransaction({writeConcern: {w: "majority"}});

res =
    sessionColl.findAndModify({query: {_id: 2}, update: {$inc: {a: 100}}, upsert: true, new: true});
assert.eq({_id: 2, a: 202}, res);
docs = sessionColl.find({}).toArray();
assert.sameMembers(docs, [{_id: 1, a: 101}, {_id: 2, a: 202}]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());

/***********************************************************************************************
 * Do a non-matching find-and-modify with upsert, requesting the old doc.
 **********************************************************************************************/

jsTest.log("Do a non-matching find-and-modify with upsert, requesting the old doc.");
session.startTransaction({writeConcern: {w: "majority"}});

res = sessionColl.findAndModify({query: {a: 3}, update: {$inc: {a: 100}}, upsert: true});
assert.eq(null, res);
docs = sessionColl.find({a: 103}, {_id: 0}).toArray();
assert.sameMembers(docs, [{a: 103}]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());

/***********************************************************************************************
 * Do a non-matching find-and-modify with upsert, requesting the new doc.
 **********************************************************************************************/

jsTest.log("Do a non-matching find-and-modify with upsert, requesting the new doc.");
session.startTransaction({writeConcern: {w: "majority"}});
res = sessionColl.findAndModify({query: {a: 4}, update: {$inc: {a: 200}}, upsert: true, new: true});

const newdoc = res;
assert.eq(204, newdoc.a);
docs = sessionColl.find({a: 204}).toArray();
assert.sameMembers(docs, [newdoc]);

// Commit the transaction.
assert.commandWorked(session.commitTransaction_forTesting());
session.endSession();
}());
