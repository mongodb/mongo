// Test transactions including multi-deletes
// @tags: [uses_transactions]
(function() {
"use strict";

load('jstests/libs/auto_retry_transaction_in_sharding.js');

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
assert.commandWorked(testColl.insert([{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1}],
                                     {writeConcern: {w: "majority"}}));

const txnOpts = {
    writeConcern: {w: "majority"}
};

jsTest.log("Do an empty multi-delete.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Remove no docs.
    let res = assert.commandWorked(sessionColl.remove({a: 99}, {justOne: false}));
    assert.eq(0, res.nRemoved);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(), [{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1}]);
}, txnOpts);

jsTest.log("Do a single-result multi-delete.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Remove one doc.
    let res = assert.commandWorked(sessionColl.remove({a: 1}, {justOne: false}));
    assert.eq(1, res.nRemoved);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(), [{_id: 0, a: 0}, {_id: 1, a: 0}]);
}, txnOpts);

jsTest.log("Do a multiple-result multi-delete.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Remove 2 docs.
    let res = assert.commandWorked(sessionColl.remove({a: 0}, {justOne: false}));
    assert.eq(2, res.nRemoved);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(), []);
}, txnOpts);

// Collection should be empty.
assert.eq(0, testColl.countDocuments({}));
}());
