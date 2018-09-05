/**
 * Fills WiredTiger cache during recovery oplog application.
 * @tags: [requires_persistence, requires_wiredtiger]
 */
(function() {
    'use strict';
    load('jstests/libs/check_log.js');

    const rst = new ReplSetTest({
        nodes: [
            {
              slowms: 30000,  // Don't log slow operations on primary.
            },
            {
              // Disallow elections on secondary.
              rsConfig: {
                  priority: 0,
                  votes: 0,
              },
              // Do not specify storage engine in this node's options because this will
              // prevent us from overriding it on restart.
            },
        ]
    });
    const nodes = rst.startSet({
        // Start with a larger storage engine cache size to allow the secondary to write
        // the oplog entries to disk. This setting will be adjusted downwards upon restart to
        // test recovery behavior as the cache fills up.
        // This overrides the --storageEngineCacheSideGB setting passed to resmoke.py but does not
        // affect the default cache size on restart.
        wiredTigerCacheSizeGB: 10,
    });
    rst.initiate();

    const primary = rst.getPrimary();
    const mydb = primary.getDB('test');
    const coll = mydb.getCollection('t');

    const numDocs = 2;
    const minDocSizeMB = 10;

    for (let i = 0; i < numDocs; ++i) {
        assert.writeOK(
            coll.save({_id: i, i: 0, x: 'x'.repeat(minDocSizeMB * 1024 * 1024)},
                      {writeConcern: {w: nodes.length, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
    }
    assert.eq(numDocs, coll.find().itcount());

    let secondary = rst.getSecondary();
    const batchOpsLimit =
        assert.commandWorked(secondary.adminCommand({getParameter: 1, replBatchLimitOperations: 1}))
            .replBatchLimitOperations;
    jsTestLog('Oplog application on secondary ' + secondary.host + ' is limited to ' +
              batchOpsLimit + ' operations per batch.');

    // Disable snapshotting on secondary so that further operations do not enter the majority
    // snapshot.
    assert.commandWorked(
        secondary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'}));

    // (3.6 only) To make the secondary replay the update operations during startup recovery, we
    // restore the pre-update state of the minvalid document before restarting the secondary.
    // As of 4.0, it is not necessary to replace the minvalid to replay oplog entries during
    // startup; it is sufficient for the purposes of this test to disable snapshotting.
    const minValidColl = secondary.getCollection('local.replset.minvalid');
    const minValidDocBeforeUpdate = minValidColl.findOne();
    jsTestLog('Minvalid document before updates: ' + tojson(minValidDocBeforeUpdate));

    const numUpdates = 500;
    jsTestLog('Writing ' + numUpdates + ' updates to ' + numDocs +
              ' documents on secondary after disabling snapshots.');
    for (let i = 0; i < numDocs; ++i) {
        for (let j = 0; j < numUpdates; ++j) {
            assert.writeOK(coll.update({_id: i}, {$inc: {i: 1}}));
        }
    }

    jsTestLog('Waiting for updates on secondary ' + secondary.host +
              ' to be written to the oplog.');
    rst.awaitReplication();

    // Overwrite the minvalid document with its pre-update values before restarting the secondary.
    const minValidDocAfterUpdate = minValidColl.findOne();
    jsTestLog('Minvalid document after updates: ' + tojson(minValidDocAfterUpdate));
    assert.writeOK(minValidColl.remove({}));
    assert.writeOK(minValidColl.save(minValidDocBeforeUpdate));
    jsTestLog('Minvalid document after replacement: ' + tojson(minValidColl.findOne()));

    secondary = rst.restart(1, {
        setParameter: {
            logComponentVerbosity: tojsononeline({replication: 2}),
        },
        // Constrain the storage engine cache size to make it easier to fill it up with unflushed
        // modification.
        wiredTigerCacheSizeGB: 1,
    });

    // Verify storage engine cache size in effect during recovery.
    const actualCacheSizeGB = assert.commandWorked(secondary.adminCommand({getCmdLineOpts: 1}))
                                  .parsed.storage.wiredTiger.engineConfig.cacheSizeGB;
    jsTestLog('Secondary was restarted with a storage cache size of ' + actualCacheSizeGB + ' GB.');
    assert.eq(1, actualCacheSizeGB);

    checkLog.contains(secondary, 'Starting recovery oplog application');
    jsTestLog('Applying updates on secondary ' + secondary.host + ' during recovery.');

    // This ensures that the node is able to complete recovery and transition to SECONDARY.
    rst.awaitReplication();

    rst.stopSet();
})();
