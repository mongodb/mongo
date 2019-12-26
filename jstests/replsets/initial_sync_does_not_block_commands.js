/**
 * Checks that the 'dbStats' and 'collStats' commands do not block during initial sync.
 *
 * @tags: [requires_replication, requires_fcv_44, requires_document_locking]
 */
(function() {
var name = 'initial_sync_does_not_block_commands';
var replSet = new ReplSetTest({
    name: name,
    nodes: 2,
});

replSet.startSet();
replSet.initiate();
var primary = replSet.getPrimary();
var secondary = replSet.getSecondary();

var coll = primary.getDB('test').getCollection(name);

var bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 250; i++) {
    bulk.insert({_id: i});
}
bulk.execute();

replSet.awaitReplication();

secondary = replSet.restart(secondary, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangDuringCollectionClone':
            tojson({mode: 'alwaysOn', data: {namespace: coll.getFullName(), numDocsToClone: 50}})
    }
});

try {
    let secondaryDB = secondary.getDB('test');
    checkLog.contains(secondaryDB,
                      "initial sync - initialSyncHangDuringCollectionClone fail point");

    assert.commandWorked(secondaryDB.runCommand({dbStats: 1}));
    assert.commandWorked(secondaryDB.runCommand({collStats: name}));
} finally {
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'initialSyncHangDuringCollectionClone', mode: 'off'}));
}

replSet.awaitReplication();
replSet.awaitSecondaryNodes();

replSet.stopSet();
})();
