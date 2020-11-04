/**
 * Test replication recovery as standalone with 'recoverToOplogTimestamp' in read-only mode
 * (i.e. queryableBackupMode).
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */

(function() {
"use strict";

const dbName = "test";
const collName = "foo";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiateWithHighElectionTimeout();
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);

const recoveryTimestamp = assert.commandWorked(primaryDB.runCommand({ping: 1})).operationTime;

// Hold back the recovery timestamp before doing another write so we have some oplog entries to
// apply when restart in queryableBackupMode with recoverToOplogTimestamp.
assert.commandWorked(primaryDB.adminCommand({
    "configureFailPoint": 'holdStableTimestampAtSpecificTimestamp',
    "mode": 'alwaysOn',
    "data": {"timestamp": recoveryTimestamp}
}));

const docs = [{_id: 1}];
const operationTime =
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: docs})).operationTime;

rst.stopSet(/*signal=*/null, /*forRestart=*/true);

// Restart as standalone in queryableBackupMode and run replication recovery up to the last insert.
const primaryStandalone = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noReplSet: true,
    noCleanData: true,
    setParameter: {recoverToOplogTimestamp: tojson({timestamp: operationTime})},
    queryableBackupMode: ""
});

// Test that the last insert is visible after replication recovery.
assert.eq(primaryStandalone.getDB(dbName)[collName].find().toArray(), docs);

MongoRunner.stopMongod(primaryStandalone);
}());
