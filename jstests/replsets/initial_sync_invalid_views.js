// Previously, the listCollections command would validate the views in all cases, potentially
// causing a secondary to crash in the initial sync of a replicate set in the case that invalid
// views were present. This test ensures that crashes no longer occur in those circumstances.

(function() {
    'use strict';

    const name = "initial_sync_invalid_views";
    let replSet = new ReplSetTest({name: name, nodes: 1});

    let oplogSizeOnPrimary = 1;  // size in MB
    replSet.startSet({oplogSize: oplogSizeOnPrimary});
    replSet.initiate();
    let primary = replSet.getPrimary();

    let coll = primary.getDB('test').foo;
    assert.writeOK(coll.insert({a: 1}));

    // Add a secondary node but make it hang before copying databases.
    let secondary = replSet.add();
    secondary.setSlaveOk();

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    replSet.reInitiate();

    assert.writeOK(primary.getDB('test').system.views.insert({invalid: NumberLong(1000)}));

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    replSet.awaitSecondaryNodes(200 * 1000);

    // Skip collection validation during stopMongod if invalid views exists.
    TestData.skipValidationOnInvalidViewDefinitions = true;

    replSet.stopSet();
})();
