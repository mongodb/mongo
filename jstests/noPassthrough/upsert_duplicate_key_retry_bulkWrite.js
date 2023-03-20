/**
 * When two concurrent identical upsert operations are performed, for which a unique index exists on
 * the query values, it is possible that they will both attempt to perform an insert with one of
 * the two failing on the unique index constraint. This test confirms that the failed insert will be
 * retried, resulting in an update.
 *
 * @tags: [requires_replication]
 */

(function() {
"use strict";

load("jstests/libs/curop_helpers.js");  // For waitForCurOpByFailPoint().

const rst = new ReplSetTest({
    nodes: {
        node0: {setParameter: {enableTestCommands: 1, featureFlagBulkWriteCommand: true}},
    }
});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB("test");
const adminDB = testDB.getSiblingDB("admin");
const collName = "upsert_duplicate_key_retry_bulkWrite";
const testColl = testDB.getCollection(collName);

testDB.runCommand({drop: collName});

function performUpsert() {
    // This function is called from startParallelShell(), so closed-over variables will not be
    // available.
    const testDB = db.getMongo().getDB("test");
    var res = testDB.adminCommand({
        bulkWrite: 1,
        ops: [
            {update: 0, filter: {x: 3}, updateMods: {$inc: {y: 1}}, upsert: true, return: "post"},
        ],
        nsInfo: [{ns: "test.upsert_duplicate_key_retry_bulkWrite"}]
    });
}

assert.commandWorked(testColl.createIndex({x: 1}, {unique: true}));

// Will hang upsert operations just prior to performing an insert.
assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: "hangBeforeBulkWritePerformsUpdate", mode: "alwaysOn"}));

const awaitUpdate1 = startParallelShell(performUpsert, rst.ports[0]);
const awaitUpdate2 = startParallelShell(performUpsert, rst.ports[0]);

// Query current operations until 2 matching operations are found.
assert.soon(() => {
    const curOps = waitForCurOpByFailPointNoNS(adminDB, "hangBeforeBulkWritePerformsUpdate");
    return curOps.length === 2;
});

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "hangBeforeBulkWritePerformsUpdate", mode: "off"}));

awaitUpdate1();
awaitUpdate2();

const cursor = testColl.find({}, {_id: 0});
assert.eq(cursor.next(), {x: 3, y: 2});
assert(!cursor.hasNext(), cursor.toArray());

// Confirm that oplog entries exist for both insert and update operation.
const oplogColl = testDB.getSiblingDB("local").getCollection("oplog.rs");
assert.eq(1,
          oplogColl.find({"op": "i", "ns": "test.upsert_duplicate_key_retry_bulkWrite"}).itcount());
assert.eq(1,
          oplogColl.find({"op": "u", "ns": "test.upsert_duplicate_key_retry_bulkWrite"}).itcount());

//
// Confirm DuplicateKey error for cases that should not be retried.
//
assert.commandWorked(testDB.runCommand({drop: collName}));
assert.commandWorked(testColl.createIndex({x: 1}, {unique: true}));

// DuplicateKey error on replacement-style upsert, where the unique index key value to be
// written does not match the value of the query predicate.
assert.commandWorked(testColl.insert({_id: 1, 'a': 12345}));
var res = testDB.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {x: 3}, updateMods: {}, upsert: true, return: "post"},
    ],
    nsInfo: [{ns: "test.upsert_duplicate_key_retry_bulkWrite"}]
});

assert(res.cursor.firstBatch[0].code == 11000);

// DuplicateKey error on update-style upsert, where the unique index key value to be written
// does not match the value of the query predicate.
assert.commandWorked(testColl.remove({}));
assert.commandWorked(testColl.insert({x: 3}));
assert.commandWorked(testColl.insert({x: 4}));
res = testDB.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {x: 3}, updateMods: {$inc: {x: 1}}, upsert: true, return: "post"},
    ],
    nsInfo: [{ns: "test.upsert_duplicate_key_retry_bulkWrite"}]
});

assert(res.cursor.firstBatch[0].code == ErrorCodes.DuplicateKey);

rst.stopSet();
})();
