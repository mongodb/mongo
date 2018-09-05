/**
 * Fills WiredTiger cache during initial sync oplog replay.
 * @tags: [requires_wiredtiger]
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
              // Constrain the storage engine cache size to make it easier to fill it up with
              // unflushed modifications.
              wiredTigerCacheSizeGB: 1,
            },
        ]
    });
    const nodes = rst.startSet();
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

    const secondary = rst.restart(1, {
        startClean: true,
        setParameter:
            'failpoint.initialSyncHangBeforeCopyingDatabases=' + tojson({mode: 'alwaysOn'})
    });

    const batchOpsLimit =
        assert.commandWorked(secondary.adminCommand({getParameter: 1, replBatchLimitOperations: 1}))
            .replBatchLimitOperations;
    jsTestLog('Oplog application on secondary ' + secondary.host + ' is limited to ' +
              batchOpsLimit + ' operations per batch.');

    const numUpdates = 500;
    jsTestLog('Buffering ' + numUpdates + ' updates to ' + numDocs + ' documents on secondary.');
    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');
    for (let i = 0; i < numDocs; ++i) {
        for (let j = 0; j < numUpdates; ++j) {
            assert.writeOK(coll.update({_id: i}, {$inc: {i: 1}}));
        }
    }

    jsTestLog('Applying updates on secondary ' + secondary.host);
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));
    rst.awaitReplication();

    rst.stopSet();
})();
