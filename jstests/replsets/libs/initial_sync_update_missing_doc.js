"use strict";
/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This library is used to delete documents on the sync source between the first two phases so
 * that the secondary will fail to apply the update operation in phase three.
 */

// reInitiate the replica set with a secondary node, which will go through initial sync. This
// function will hang the secondary in initial sync. turnOffHangBeforeCopyingDatabasesFailPoint
// must be called after reInitiateSetWithSecondary.
var reInitiateSetWithSecondary = function(replSet, secondaryConfig) {
    const secondary = replSet.add(secondaryConfig);
    secondary.setSlaveOk();

    // Make the secondary hang after retrieving the last op on the sync source but before
    // copying databases.
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));

    // Skip clearing initial sync progress after a successful initial sync attempt so that we
    // can check initialSyncStatus fields after initial sync is complete.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'skipClearInitialSyncState', mode: 'alwaysOn'}));

    replSet.reInitiate();

    // Wait for fail point message to be logged.
    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    return secondary;
};

// Must be called after reInitiateSetWithSecondary. Turns off the
// initialSyncHangBeforeCopyingDatabases fail point so that the secondary will start copying all
// non-local databases.
var turnOffHangBeforeCopyingDatabasesFailPoint = function(secondary) {
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));
};

var finishAndValidate = function(replSet, name, numDocuments) {
    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();
    const dbName = 'test';
    const primary = replSet.getPrimary();
    const primaryCollection = primary.getDB(dbName).getCollection(name);
    const secondary = replSet.getSecondary();
    const secondaryCollection = secondary.getDB(dbName).getCollection(name);

    if (numDocuments != secondaryCollection.find().itcount()) {
        jsTestLog(`Mismatch, primary collection: ${tojson(primaryCollection.find().toArray())}
secondary collection: ${tojson(secondaryCollection.find().toArray())}`);
        throw new Error('Did not sync collection');
    }

    assert.eq(0,
              secondary.getDB('local')['temp_oplog_buffer'].find().itcount(),
              "Oplog buffer was not dropped after initial sync");
};

var updateRemove = function(sessionColl, query) {
    assert.commandWorked(sessionColl.update(query, {x: 2}, {upsert: false}));
    assert.commandWorked(sessionColl.remove(query, {justOne: true}));
};

var insertUpdateRemoveLarge = function(sessionColl, query) {
    const kSize10MB = 10 * 1024 * 1024;
    const longString = "a".repeat(kSize10MB);
    assert.commandWorked(sessionColl.insert({x: longString}));
    assert.commandWorked(sessionColl.update(query, {x: longString}, {upsert: false}));
    assert.commandWorked(sessionColl.remove(query, {justOne: true}));
};
