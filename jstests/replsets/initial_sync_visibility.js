// SERVER-30927 Initial sync from a source with an invisible oplog entry
(function() {
    'use strict';

    load('jstests/replsets/rslib.js');
    const basename = 'initial_sync_visibility';

    jsTestLog('Bring up set');
    const rst = new ReplSetTest({name: basename, nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(basename);

    jsTestLog('Create a collection');
    assert.writeOK(primaryDB['coll'].save({_id: "visible"}));
    jsTestLog('Make sure synced');
    rst.awaitReplication();

    jsTestLog('Activate WT visibility failpoint and write an invisible document');
    assert.commandWorked(primaryDB.adminCommand(
        {configureFailPoint: 'WTPausePrimaryOplogDurabilityLoop', mode: 'alwaysOn'}));
    assert.writeOK(primaryDB['coll'].save({_id: "invisible"}));

    jsTestLog('Bring up a new node');
    const secondary = rst.add({setParameter: 'numInitialSyncAttempts=3'});
    rst.reInitiate();
    assert.eq(primary, rst.getPrimary(), 'Primary changed after reconfig');

    jsTestLog('Wait for new node to start cloning');
    secondary.setSlaveOk();
    const secondaryDB = secondary.getDB(basename);
    wait(function() {
        return secondaryDB.stats().collections >= 1;
    }, 'never saw new node starting to clone, was waiting for collections in: ' + basename);

    jsTestLog('Disable WT visibility failpoint on primary making all visible.');
    assert.commandWorked(primaryDB.adminCommand(
        {configureFailPoint: 'WTPausePrimaryOplogDurabilityLoop', mode: 'off'}));

    jsTestLog('Wait for both nodes to be up-to-date');
    rst.awaitSecondaryNodes();
    rst.awaitReplication();

    jsTestLog('Check all OK');
    rst.checkReplicatedDataHashes();
    rst.stopSet(15);
})();
