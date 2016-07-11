/*
 * This test validates command line parameter parsing for initial sync. It tests that the
 * initialSyncOplogBuffer can only use a collection when using the data replicator. It then checks
 * that only valid initialSyncOplogBuffer options are accepted. Finally it checks that valid
 * combinations of use3dot2InitialSync and initialSyncOplogBuffer are parsed properly.
 */

(function() {
    "use strict";
    // If the parameter is already set, don't run this test.
    var parameters = db.adminCommand({getCmdLineOpts: 1}).parsed.setParameter;
    if (parameters.use3dot2InitialSync || parameters.initialSyncOplogBuffer) {
        jsTest.log("Skipping initial_sync_parameters.js because use3dot2InitialSync or " +
                   "initialSyncOplogBuffer was already provided.");
        return;
    }

    var m = MongoRunner.runMongod(
        {setParameter: {use3dot2InitialSync: true, initialSyncOplogBuffer: 'collection'}});
    assert.eq(m, null);

    m = MongoRunner.runMongod(
        {setParameter: {use3dot2InitialSync: false, initialSyncOplogBuffer: 'invalid'}});
    assert.eq(m, null);

    var m2 = MongoRunner.runMongod({
        setParameter:
            {use3dot2InitialSync: true, initialSyncOplogBuffer: 'inMemoryBlockingQueue'}
    });
    var res = assert.commandWorked(
        m2.adminCommand({getParameter: 1, use3dot2InitialSync: 1, initialSyncOplogBuffer: 1}));
    assert.eq(res.use3dot2InitialSync, true);
    assert.eq(res.initialSyncOplogBuffer, "inMemoryBlockingQueue");
    MongoRunner.stopMongod(m2);

    var m3 = MongoRunner.runMongod(
        {setParameter: {use3dot2InitialSync: false, initialSyncOplogBuffer: 'collection'}});
    res = assert.commandWorked(
        m3.adminCommand({getParameter: 1, use3dot2InitialSync: 1, initialSyncOplogBuffer: 1}));
    assert.eq(res.use3dot2InitialSync, false);
    assert.eq(res.initialSyncOplogBuffer, "collection");
    MongoRunner.stopMongod(m3);

    var m4 = MongoRunner.runMongod({
        setParameter:
            {use3dot2InitialSync: false, initialSyncOplogBuffer: 'inMemoryBlockingQueue'}
    });
    res = assert.commandWorked(
        m4.adminCommand({getParameter: 1, use3dot2InitialSync: 1, initialSyncOplogBuffer: 1}));
    assert.eq(res.use3dot2InitialSync, false);
    assert.eq(res.initialSyncOplogBuffer, "inMemoryBlockingQueue");

    assert.commandFailed(m4.adminCommand({setParameter: 1, use3dot2InitialSync: true}));
    assert.commandFailed(m4.adminCommand({setParameter: 1, initialSyncOplogBuffer: "collection"}));

    MongoRunner.stopMongod(m4);
})();
