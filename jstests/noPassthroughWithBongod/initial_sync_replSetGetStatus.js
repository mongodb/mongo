/**
 * This test tests that replSetGetStatus returns initial sync stats while initial sync is in
 * progress.
 */

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    // If the parameter is already set, don't run this test.
    var parameters = db.adminCommand({getCmdLineOpts: 1}).parsed.setParameter;
    if (parameters.use3dot2InitialSync || parameters.initialSyncOplogBuffer) {
        jsTest.log("Skipping initial_sync_parameters.js because use3dot2InitialSync or " +
                   "initialSyncOplogBuffer was already provided.");
        return;
    }

    var name = 'initial_sync_replSetGetStatus';
    var replSet = new ReplSetTest({
        name: name,
        nodes: 1,
    });

    replSet.startSet();
    replSet.initiate();
    var primary = replSet.getPrimary();

    var coll = primary.getDB('test').foo;
    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({a: 2}));

    // Add a secondary node but make it hang before copying databases.
    var secondary = replSet.add(
        {setParameter: {use3dot2InitialSync: false, initialSyncOplogBuffer: "collection"}});
    secondary.setSlaveOk();

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeFinish', mode: 'alwaysOn'}));
    replSet.reInitiate();

    // Wait for initial sync to pause before it copies the databases.
    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    // Test that replSetGetStatus returns the correct results while initial sync is in progress.
    var res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
    assert(!res.initialSyncStatus,
           "Response should not have an 'initialSyncStatus' field: " + tojson(res));

    res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
    assert(res.initialSyncStatus,
           "Response should have an 'initialSyncStatus' field: " + tojson(res));

    assert.commandFailed(secondary.adminCommand({replSetGetStatus: 1, initialSync: "t"}),
                         ErrorCodes.TypeMismatch);

    assert.writeOK(coll.insert({a: 3}));
    assert.writeOK(coll.insert({a: 4}));

    // Let initial sync continue working.
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    // Wait for initial sync to pause right before it finishes.
    checkLog.contains(secondary, 'initial sync - initialSyncHangBeforeFinish fail point enabled');

    // Test that replSetGetStatus returns the correct results when initial sync is at the very end.
    res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
    assert(res.initialSyncStatus, "Response should have an 'initialSyncStatus' field.");
    assert.eq(res.initialSyncStatus.fetchedMissingDocs, 0);
    assert.eq(res.initialSyncStatus.appliedOps, 2);
    assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 0);
    assert.eq(res.initialSyncStatus.maxFailedInitialSyncAttempts, 1);
    assert.eq(res.initialSyncStatus.databases.databasesCloned, 2);
    assert.eq(res.initialSyncStatus.databases.test.collections, 1);
    assert.eq(res.initialSyncStatus.databases.test.clonedCollections, 1);
    assert.eq(res.initialSyncStatus.databases.test["test.foo"].documentsToCopy, 4);
    assert.eq(res.initialSyncStatus.databases.test["test.foo"].documentsCopied, 4);
    assert.eq(res.initialSyncStatus.databases.test["test.foo"].indexes, 1);
    assert.eq(res.initialSyncStatus.databases.test["test.foo"].fetchedBatches, 1);

    // Let initial sync finish and get into secondary state.
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeFinish', mode: 'off'}));
    replSet.awaitSecondaryNodes(60 * 1000);

    // Test that replSetGetStatus returns the correct results after initial sync is finished.
    res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
    assert(!res.initialSyncStatus,
           "Response should not have an 'initialSyncStatus' field: " + tojson(res));

    assert.commandFailedWithCode(secondary.adminCommand({replSetGetStatus: 1, initialSync: "m"}),
                                 ErrorCodes.TypeMismatch);
    assert.eq(0,
              secondary.getDB('local')['temp_oplog_buffer'].find().itcount(),
              "Oplog buffer was not dropped after initial sync");
})();
