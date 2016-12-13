/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source and fetches operations from sync source; and
 * 3) applies operations from the source after op_start1.
 *
 * This test renames a collection on the source between phases 1 and 2, but renameCollection is not
 * supported in initial sync. The secondary will initially fail to apply the command in phase 3
 * and subsequently have to retry the initial sync.
 */

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    var parameters = TestData.setParameters;
    if (parameters && parameters.indexOf("use3dot2InitialSync=true") != -1) {
        jsTest.log("Skipping this test because use3dot2InitialSync was provided.");
        return;
    }

    var name = 'initial_sync_applier_error';
    var replSet = new ReplSetTest({
        name: name,
        nodes: [{}, {rsConfig: {arbiterOnly: true}}],
    });

    replSet.startSet();
    replSet.initiate();
    var primary = replSet.getPrimary();

    var coll = primary.getDB('test').getCollection(name);
    assert.writeOK(coll.insert({_id: 0, content: "hi"}));

    // Add a secondary node but make it hang after retrieving the last op on the source
    // but before copying databases.
    var secondary = replSet.add({setParameter: "numInitialSyncAttempts=2"});
    secondary.setSlaveOk();

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    replSet.reInitiate();

    // Wait for fail point message to be logged.
    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    var newCollName = name + '_2';
    assert.commandWorked(coll.renameCollection(newCollName, true));
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    checkLog.contains(secondary, 'Applying renameCollection not supported');
    checkLog.contains(secondary, 'initial sync done');

    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();

    assert.eq(0, secondary.getDB('test').getCollection(name).count());
    assert.eq(1, secondary.getDB('test').getCollection(newCollName).count());
    assert.eq("hi", secondary.getDB('test').getCollection(newCollName).findOne({_id: 0}).content);
})();
