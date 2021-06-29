// Test transactions including multi-updates.
// @tags: [uses_transactions]
(function() {
"use strict";

load('jstests/libs/auto_retry_transaction_in_sharding.js');

const dbName = "test";
const collName = "multi_update_in_transaction";
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

jsTest.log("Do an empty multi-update.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Update 0 docs.
    let res = assert.commandWorked(sessionColl.update({a: 99}, {$set: {b: 1}}, {multi: true}));
    assert.eq(0, res.nModified);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(), [{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1}]);
}, txnOpts);

jsTest.log("Do a single-result multi-update.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Update 1 doc.
    let res = assert.commandWorked(sessionColl.update({a: 1}, {$set: {b: 1}}, {multi: true}));
    assert.eq(1, res.nModified);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(), [{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1, b: 1}]);
}, txnOpts);

jsTest.log("Do a multiple-result multi-update.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Update 2 docs.
    let res = assert.commandWorked(sessionColl.update({a: 0}, {$set: {b: 2}}, {multi: true}));
    assert.eq(2, res.nModified);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(),
                       [{_id: 0, a: 0, b: 2}, {_id: 1, a: 0, b: 2}, {_id: 2, a: 1, b: 1}]);
}, txnOpts);

jsTest.log("Do a multiple-query multi-update.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Bulk update 3 docs.
    let bulk = sessionColl.initializeUnorderedBulkOp();
    bulk.find({a: 0}).update({$set: {c: 1}});
    bulk.find({_id: 2}).update({$set: {c: 2}});
    let res = assert.commandWorked(bulk.execute());
    assert.eq(3, res.nModified);

    res = sessionColl.find({});
    assert.sameMembers(
        res.toArray(),
        [{_id: 0, a: 0, b: 2, c: 1}, {_id: 1, a: 0, b: 2, c: 1}, {_id: 2, a: 1, b: 1, c: 2}]);
}, txnOpts);

jsTest.log("Do a multi-update with upsert.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Upsert 1 doc.
    let res = assert.commandWorked(
        sessionColl.update({_id: 3}, {$set: {d: 1}}, {multi: true, upsert: true}));
    assert.eq(1, res.nUpserted);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(), [
        {_id: 0, a: 0, b: 2, c: 1},
        {_id: 1, a: 0, b: 2, c: 1},
        {_id: 2, a: 1, b: 1, c: 2},
        {_id: 3, d: 1}
    ]);
}, txnOpts);
}());
