/*
 * This test validates command line parameter parsing for initial sync. It tests that the
 * initialSyncOplogBuffer can only use a collection when using the data replicator. It then checks
 * that only valid initialSyncOplogBuffer options are accepted. Finally it checks that valid
 * combinations of useDataReplicatorInitialSync and initialSyncOplogBuffer are parsed properly.
 */

(function() {
    "use strict";
    var m = MongoRunner.runMongod({
        setParameter: {useDataReplicatorInitialSync: false, initialSyncOplogBuffer: 'collection'}
    });
    assert.eq(m, null);

    m = MongoRunner.runMongod(
        {setParameter: {useDataReplicatorInitialSync: true, initialSyncOplogBuffer: 'invalid'}});
    assert.eq(m, null);

    var m2 = MongoRunner.runMongod({
        setParameter: {
            useDataReplicatorInitialSync: false,
            initialSyncOplogBuffer: 'inMemoryBlockingQueue'
        }
    });
    var res = assert.commandWorked(m2.adminCommand(
        {getParameter: 1, useDataReplicatorInitialSync: 1, initialSyncOplogBuffer: 1}));
    assert.eq(res.useDataReplicatorInitialSync, false);
    assert.eq(res.initialSyncOplogBuffer, "inMemoryBlockingQueue");
    MongoRunner.stopMongod(m2);

    var m3 = MongoRunner.runMongod(
        {setParameter: {useDataReplicatorInitialSync: true, initialSyncOplogBuffer: 'collection'}});
    res = assert.commandWorked(m3.adminCommand(
        {getParameter: 1, useDataReplicatorInitialSync: 1, initialSyncOplogBuffer: 1}));
    assert.eq(res.useDataReplicatorInitialSync, true);
    assert.eq(res.initialSyncOplogBuffer, "collection");
    MongoRunner.stopMongod(m3);

    var m4 = MongoRunner.runMongod({
        setParameter:
            {useDataReplicatorInitialSync: true, initialSyncOplogBuffer: 'inMemoryBlockingQueue'}
    });
    res = assert.commandWorked(m4.adminCommand(
        {getParameter: 1, useDataReplicatorInitialSync: 1, initialSyncOplogBuffer: 1}));
    assert.eq(res.useDataReplicatorInitialSync, true);
    assert.eq(res.initialSyncOplogBuffer, "inMemoryBlockingQueue");

    assert.commandFailed(m4.adminCommand({setParameter: 1, useDataReplicatorInitialSync: false}));
    assert.commandFailed(m4.adminCommand({setParameter: 1, initialSyncOplogBuffer: "collection"}));

    MongoRunner.stopMongod(m4);
})();
