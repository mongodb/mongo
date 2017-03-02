/**
 * SERVER-20617: Tests that journaled write operations survive a kill -9 of the bongod.
 *
 * This test requires persistence to ensure data survives a restart.
 * @tags: [requires_persistence]
 */
(function() {
    'use strict';

    //  The following test verifies that writeConcern: {j: true} ensures that data is durable.
    var dbpath = BongoRunner.dataPath + 'sync_write';
    resetDbpath(dbpath);

    var bongodArgs = {dbpath: dbpath, noCleanData: true, journal: ''};

    // Start a bongod.
    var conn = BongoRunner.runBongod(bongodArgs);
    assert.neq(null, conn, 'bongod was unable to start up');

    // Now connect to the bongod, do a journaled write and abruptly stop the server.
    var testDB = conn.getDB('test');
    assert.writeOK(testDB.synced.insert({synced: true}, {writeConcern: {j: true}}));
    BongoRunner.stopBongod(conn, 9);

    // Restart the bongod.
    conn = BongoRunner.runBongod(bongodArgs);
    assert.neq(null, conn, 'bongod was unable to restart after receiving a SIGKILL');

    // Check that our journaled write still is present.
    testDB = conn.getDB('test');
    assert.eq(1, testDB.synced.count({synced: true}), 'synced write was not found');
    BongoRunner.stopBongod(conn);
})();
