/**
 * Tests that renameCollection commands do not abort initial sync when users specify
 * 'allowUnsafeRenamesDuringInitialSync'.
 */

(function() {
    'use strict';

    load("jstests/libs/check_log.js");

    var parameters = TestData.setParameters;
    if (parameters && parameters.indexOf("use3dot2InitialSync=true") != -1) {
        jsTest.log("Skipping this test because use3dot2InitialSync was provided.");
        return;
    }

    const basename = 'initial_sync_rename_collection_unsafe';

    const rst = new ReplSetTest({name: basename, nodes: 1});
    rst.startSet();
    rst.initiate();

    const dbName = 'd';
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.writeOK(primaryDB['foo'].save({}));

    jsTestLog('Bring up a new node');
    const secondary = rst.add({setParameter: {allowUnsafeRenamesDuringInitialSync: true}});
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    rst.reInitiate();
    assert.eq(primary, rst.getPrimary(), 'Primary changed after reconfig');

    // Wait for fail point message to be logged.
    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    jsTestLog('Rename collection on the primary');
    assert.commandWorked(primaryDB['foo'].renameCollection('renamed'));

    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    checkLog.contains(secondary, 'allowUnsafeRenamesDuringInitialSync set to true');

    jsTestLog('Wait for both nodes to be up-to-date');
    rst.awaitSecondaryNodes();
    rst.awaitReplication();

    jsTestLog('Check that all collections were renamed correctly on the secondary');
    const secondaryDB = secondary.getDB(dbName);
    assert.eq(secondaryDB['renamed'].find().itcount(), 1, 'renamed collection does not exist');
    assert.eq(secondaryDB['foo'].find().itcount(), 0, 'collection `foo` exists after rename');

    let res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
    assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 0);

    rst.stopSet();
})();
