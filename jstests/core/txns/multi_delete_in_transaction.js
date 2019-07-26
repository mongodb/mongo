// Test transactions including multi-deletes
// @tags: [uses_transactions]
(function() {
"use strict";

const dbName = "test";
const collName = "multi_delete_in_transaction";
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
assert.writeOK(testColl.insert([{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1}],
                               {writeConcern: {w: "majority"}}));

jsTest.log("Do an empty multi-delete.");
session.startTransaction({writeConcern: {w: "majority"}});

// Remove no docs.
let res = sessionColl.remove({a: 99}, {justOne: false});
assert.eq(0, res.nRemoved);
res = sessionColl.find({});
assert.sameMembers(res.toArray(), [{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1}]);

assert.commandWorked(session.commitTransaction_forTesting());

jsTest.log("Do a single-result multi-delete.");
session.startTransaction({writeConcern: {w: "majority"}});

// Remove one doc.
res = sessionColl.remove({a: 1}, {justOne: false});
assert.eq(1, res.nRemoved);
res = sessionColl.find({});
assert.sameMembers(res.toArray(), [{_id: 0, a: 0}, {_id: 1, a: 0}]);

assert.commandWorked(session.commitTransaction_forTesting());

jsTest.log("Do a multiple-result multi-delete.");
session.startTransaction({writeConcern: {w: "majority"}});

// Remove 2 docs.
res = sessionColl.remove({a: 0}, {justOne: false});
assert.eq(2, res.nRemoved);
res = sessionColl.find({});
assert.sameMembers(res.toArray(), []);

assert.commandWorked(session.commitTransaction_forTesting());

// Collection should be empty.
assert.eq(0, testColl.find().itcount());
}());
