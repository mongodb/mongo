/**
 * SERVER-20617: Tests that journaled write operations survive a kill -9 of the merizod.
 *
 * This test requires persistence to ensure data survives a restart.
 * @tags: [requires_persistence]
 */
(function() {
    'use strict';

    //  The following test verifies that writeConcern: {j: true} ensures that data is durable.
    var dbpath = MerizoRunner.dataPath + 'sync_write';
    resetDbpath(dbpath);

    var merizodArgs = {dbpath: dbpath, noCleanData: true, journal: ''};

    // Start a merizod.
    var conn = MerizoRunner.runMerizod(merizodArgs);
    assert.neq(null, conn, 'merizod was unable to start up');

    // Now connect to the merizod, do a journaled write and abruptly stop the server.
    var testDB = conn.getDB('test');
    assert.writeOK(testDB.synced.insert({synced: true}, {writeConcern: {j: true}}));
    MerizoRunner.stopMerizod(conn, 9, {allowedExitCode: MerizoRunner.EXIT_SIGKILL});

    // Restart the merizod.
    conn = MerizoRunner.runMerizod(merizodArgs);
    assert.neq(null, conn, 'merizod was unable to restart after receiving a SIGKILL');

    // Check that our journaled write still is present.
    testDB = conn.getDB('test');
    assert.eq(1, testDB.synced.count({synced: true}), 'synced write was not found');
    MerizoRunner.stopMerizod(conn);
})();
