/**
 * This tests that writes with majority write concern will wait for at least the all durable
 * timestamp to reach the timestamp of the write. This guarantees that once a write is majority
 * committed, reading at the all durable timestamp will read that write.
 *
 * @tags: [incompatible_with_eft]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function assertWriteConcernTimeout(result) {
    assert.writeErrorWithCode(result, ErrorCodes.WriteConcernFailed);
    assert(result.hasWriteConcernError(), tojson(result));
    assert(result.getWriteConcernError().errInfo.wtimeout, tojson(result));
}

const rst = new ReplSetTest({name: "writes_wait_for_all_durable", nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = "majority_writes_wait_for_all_durable";
const testDB = primary.getDB(dbName);
const testColl = testDB[collName];

TestData.dbName = dbName;
TestData.collName = collName;

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

const failPoint = configureFailPoint(
    testDB, "hangAfterCollectionInserts", {collectionNS: testColl.getFullName(), first_id: "b"});

jsTestLog(
    "Insert a document to hang before the insert completes to hold back the all durable timestamp.");
const joinHungWrite = startParallelShell(() => {
    assert.commandWorked(db.getSiblingDB(TestData.dbName)[TestData.collName].insert({_id: "b"}));
}, primary.port);
jsTestLog("Checking that the log contains fail point enabled.");
failPoint.wait();

try {
    jsTest.log("Do a write with majority write concern that should time out.");
    // Note: we must use {j: false} otherwise the command can hang where writeConcern waits for no
    // oplog holes, which currently does not obey wtimeout (SERVER-46191), before persistence on
    // single voter replica set primaries.
    assertWriteConcernTimeout(
        testColl.insert({_id: 0}, {writeConcern: {w: "majority", j: false, wtimeout: 2 * 1000}}));
} finally {
    failPoint.off();
}

joinHungWrite();
rst.stopSet();
})();
