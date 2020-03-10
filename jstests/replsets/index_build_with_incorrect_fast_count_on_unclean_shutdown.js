/*
 * Test that index builder doesn't skip building index when the fast count value is incorrect
 * on unclean shutdowns.
 *
 *  @tags: [requires_persistence,
 *          # Restarting a node after an unclean shutdown is not supported in multiversion testing.
 *          multiversion_incompatible]
 */
(function() {

"use strict";
load("jstests/replsets/rslib.js");
load("jstests/libs/storage_helpers.js");  // getOldestRequiredTimestampForCrashRecovery()

// Set the syncdelay to a small value to increase checkpoint frequency.
var rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}], nodeOptions: {syncdelay: 1}});
rst.startSet();
rst.initiate();

const dbName = jsTest.name();
const collName = "coll";

let primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryAdmin = primary.getDB("admin");
let primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();

jsTestLog("Do some document writes.");
const InsertWriteTimestamp = assert
                                 .commandWorked(primaryColl.runCommand("insert", {
                                     documents: [{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}],
                                     writeConcern: {w: "majority"}
                                 }))
                                 .operationTime;
rst.awaitReplication();

// Wait for checkpoint to include the above writes.
jsTestLog("Waiting for stable checkpoint to include the write timestamp " + InsertWriteTimestamp);
assert.soon(
    () => {
        const oldestRequiredTimestampForCrashRecovery =
            getOldestRequiredTimestampForCrashRecovery(primaryAdmin);
        return bsonWoCompare(oldestRequiredTimestampForCrashRecovery, InsertWriteTimestamp) >= 0;
    },
    () => {
        return "Checkpoint failed to include the writes. Checkpoint timestamp: " +
            tojson(getOldestRequiredTimestampForCrashRecovery(primaryAdmin));
    });

// Do a unclean shutdown, so that size info (fast count) for collection "coll" is not flushed to
// disk.
jsTestLog("Restarting the primary node - Unclean shutdown.");
rst.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
rst.start(0, {}, true /* restart */);

// Wait for the restarted node to get elected.
primary = rst.getPrimary();
primaryColl = primary.getDB(dbName)[collName];

// Check if the fast count for "coll" is not reflecting the above writes.
assert.eq(primaryColl.count(), 0);

// Index build should have generated index entries for the above writes.
jsTestLog("Create index.");
assert.commandWorked(primaryColl.createIndex({"x": 1}));

// Collection validation on "coll" should not fail with missing index entries.
rst.stopSet();
})();
