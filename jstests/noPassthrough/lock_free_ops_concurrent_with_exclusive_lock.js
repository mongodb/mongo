/**
 * Tests that find, count, distinct and aggregation commands can run while a MODE_X collection lock
 * is held.
 *
 * @tags: [
 *     # Cannot run against older binaries because they do not have lock-free ops.
 *     requires_fcv_49,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

let conn = MongoRunner.runMongod({});
assert(conn);

// Take storageEngine setting into account when checking featureFlagLockFreeReads as
// ephemeralForTest automatically uses enableMajorityReadConcern=false and will disable the feature
// even when the flag is enabled.
const isLockFreeReadsEnabled = conn.adminCommand({getParameter: 1, featureFlagLockFreeReads: 1})
                                   .featureFlagLockFreeReads.value &&
    jsTest.options().storageEngine !== "ephemeralForTest";

if (!isLockFreeReadsEnabled) {
    jsTestLog("Exiting test because lock-free reads are not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = 'testDatabase';
const collName = 'testCollection';

const db = conn.getDB(dbName);
const coll = db.getCollection(collName);

// Set up data to read.
assert.commandWorked(coll.insert({_id: 1, topGroupId: "A", subGroupCount: 4}));
assert.commandWorked(coll.insert({_id: 2, topGroupId: "B", subGroupCount: 7}));
assert.commandWorked(coll.insert({_id: 3, topGroupId: "A", subGroupCount: 11}));

jsTestLog("Setting failpoint to block collMod operation after lock acquisition.");
const collModFailPointName = 'hangAfterDatabaseLock';  // Takes a coll MODE_X, database MODE_IX
let collModFailPoint = configureFailPoint(conn, collModFailPointName);

jsTestLog("Starting collMod that will hang after lock acquisition.");
const awaitBlockingCollMod = startParallelShell(() => {
    // Runs a no-op collMod command.
    assert.commandWorked(db.getSiblingDB('testDatabase').runCommand({collMod: 'testCollection'}));
}, conn.port);

jsTestLog("Waiting for collMod to acquire a database lock.");
collModFailPoint.wait();

jsTestLog("Starting lock-free find command.");
// Set a batch size of 1, so there are more documents for the subsequent getMore command to fetch.
const findResult = db.runCommand({find: collName, batchSize: 1});
assert.commandWorked(findResult);
assert.eq(1, findResult.cursor.firstBatch.length);

jsTestLog("Starting lock-free getMore command");
const getMoreResult =
    db.runCommand({getMore: NumberLong(findResult.cursor.id), collection: collName, batchSize: 1});
assert.commandWorked(getMoreResult);
assert.eq(1, getMoreResult.cursor.nextBatch.length);

jsTestLog("Starting lock-free count command.");
const countResult = coll.find().count();
assert.eq(3, countResult);

jsTestLog("Starting lock-free distinct command.");
const distinctResult = coll.distinct("topGroupId");
assert.eq(["A", "B"], distinctResult.sort());

jsTestLog("Starting lock-free aggregation command.");
const aggregationResult = coll.aggregate([
    {$match: {topGroupId: "A"}},
    {$group: {_id: "$topGroupId", totalTopGroupCount: {$sum: "$subGroupCount"}}}
]);
const aggregationDocuments = aggregationResult.toArray();
assert.eq(1, aggregationDocuments.length);
assert.eq(15, aggregationDocuments[0].totalTopGroupCount);

jsTestLog("Turning off failpoint.");
collModFailPoint.off();

jsTestLog("Waiting for unstalled collMod operation to finish.");
awaitBlockingCollMod();

jsTestLog("Done.");
MongoRunner.stopMongod(conn);
})();
