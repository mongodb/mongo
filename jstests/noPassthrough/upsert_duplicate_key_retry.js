/**
 * When two concurrent identical upsert operations are performed, for which a unique index exists on
 * the query values, it is possible that they will both attempt to perform an insert with one of
 * the two failing on the unique index constraint. This test confirms that the failed insert will be
 * retried, resulting in an update.
 *
 * In order for one of the two conflicting upserts to make progress we require a storage engine
 * which supports document level locking.
 *  @tags: [requires_replication, requires_document_locking]
 */

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB("test");
const adminDB = testDB.getSiblingDB("admin");
const collName = "upsert_duplicate_key_retry";
const testColl = testDB.getCollection(collName);

testDB.runCommand({drop: collName});

// Queries current operations until 'count' matching operations are found.
function awaitMatchingCurrentOpCount(message, count) {
    assert.soon(() => {
        const currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: {msg: message}}]).toArray();
        return (currentOp.length === count);
    });
}

function performUpsert() {
    // This function is called from startParallelShell(), so closed-over variables will not be
    // available. We must re-obtain the value of 'testColl' in the function body.
    const testColl = db.getMongo().getDB("test").getCollection("upsert_duplicate_key_retry");
    assert.commandWorked(testColl.update({x: 3}, {$inc: {y: 1}}, {upsert: true}));
}

assert.commandWorked(testColl.createIndex({x: 1}, {unique: true}));

// Will hang upsert operations just prior to performing an insert.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "hangBeforeUpsertPerformsInsert", mode: "alwaysOn"}));

const awaitUpdate1 = startParallelShell(performUpsert, rst.ports[0]);
const awaitUpdate2 = startParallelShell(performUpsert, rst.ports[0]);

awaitMatchingCurrentOpCount("hangBeforeUpsertPerformsInsert", 2);

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "hangBeforeUpsertPerformsInsert", mode: "off"}));

awaitUpdate1();
awaitUpdate2();

const cursor = testColl.find({}, {_id: 0});
assert.eq(cursor.next(), {x: 3, y: 2});
assert(!cursor.hasNext(), cursor.toArray());

// Confirm that oplog entries exist for both insert and update operation.
const oplogColl = testDB.getSiblingDB("local").getCollection("oplog.rs");
assert.eq(1, oplogColl.find({"op": "i", "ns": "test.upsert_duplicate_key_retry"}).itcount());
assert.eq(1, oplogColl.find({"op": "u", "ns": "test.upsert_duplicate_key_retry"}).itcount());

//
// Confirm DuplicateKey error for cases that should not be retried.
//
assert.commandWorked(testDB.runCommand({drop: collName}));
assert.commandWorked(testColl.createIndex({x: 1}, {unique: true}));

// DuplicateKey error on replacement-style upsert, where the unique index key value to be
// written does not match the value of the query predicate.
assert.commandWorked(testColl.createIndex({x: 1}, {unique: true}));
assert.commandWorked(testColl.insert({_id: 1, 'a': 12345}));
assert.commandFailedWithCode(testColl.update({x: 3}, {}, {upsert: true}), ErrorCodes.DuplicateKey);

// DuplicateKey error on update-style upsert, where the unique index key value to be written
// does not match the value of the query predicate.
assert.commandWorked(testColl.remove({}));
assert.commandWorked(testColl.insert({x: 3}));
assert.commandWorked(testColl.insert({x: 4}));
assert.commandFailedWithCode(testColl.update({x: 3}, {$inc: {x: 1}}, {upsert: true}),
                             ErrorCodes.DuplicateKey);

rst.stopSet();
})();
