// SERVER-4941 Test collection rename during initial sync

(function() {
    'use strict';

    load('jstests/replsets/rslib.js');
    const basename = 'initial_sync_rename_collection';
    const numColls = 100;

    jsTestLog('Bring up set');
    const rst = new ReplSetTest({name: basename, nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB('d');
    const primaryExtraDB = primary.getDB('e');

    jsTestLog('Create a bunch of collections');
    for (let i = 0; i < numColls; ++i) {
        assert.writeOK(primaryDB['c' + i].save({}));
    }
    assert.writeOK(primaryExtraDB['renameAcrossDatabases'].save({}));

    jsTestLog('Make sure synced');
    rst.awaitReplication();

    jsTestLog('Bring up a new node');
    const secondary = rst.add({setParameter: 'numInitialSyncAttempts=1'});

    jsTestLog('Begin initial sync on secondary');
    let conf = rst.getPrimary().getDB('admin').runCommand({replSetGetConfig: 1}).config;
    conf.members.push({_id: 1, host: secondary.host, priority: 0, votes: 0});
    conf.version++;
    assert.commandWorked(rst.getPrimary().getDB('admin').runCommand({replSetReconfig: conf}));
    assert.eq(primary, rst.getPrimary(), 'Primary changed after reconfig');

    jsTestLog('Wait for new node to start cloning');
    secondary.setSlaveOk();
    const secondaryDB = secondary.getDB('d');
    const secondaryExtraDB = secondary.getDB('e');
    wait(function() {
        return secondaryDB.stats().collections >= 1;
    }, 'never saw new node starting to clone, was waiting for docs in ' + secondaryDB['c0']);

    jsTestLog('Rename collections on the primary');
    const lastCollName = 'c' + (numColls - 1);
    assert.commandWorked(primaryDB[lastCollName].renameCollection('renamed'));
    assert.commandWorked(primaryExtraDB.adminCommand({
        renameCollection: primaryExtraDB['renameAcrossDatabases'].getFullName(),
        to: primaryDB[lastCollName].getFullName()
    }));

    jsTestLog('Wait for both nodes to be up-to-date');
    rst.awaitSecondaryNodes();
    rst.awaitReplication();

    jsTestLog('Check that all collections where renamed correctly on the secondary');
    assert.eq(secondaryDB['renamed'].find().itcount(), 1, 'renamed collection does not exist');
    assert.eq(secondaryDB[lastCollName].find().itcount(),
              1,
              'collection ' + lastCollName + ' expected to exist after rename across databases');
    assert.eq(secondaryExtraDB['renameAcrossDatabases'].find().itcount(),
              0,
              'collection RenameAcrossDatabases still exists after it was supposed to be renamed');

    rst.checkReplicatedDataHashes();
    rst.stopSet(15);
})();
