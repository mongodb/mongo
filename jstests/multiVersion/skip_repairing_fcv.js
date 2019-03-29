/**
 * Tests that running repair with a 4.0 binary on 3.4 data files avoids crashing mongod if any of
 * the collections do not have UUIDs and that it exits repair cleanly.
 *
 * @tags: [requires_persistence, requires_wiredtiger]
 */
(function() {
    'use strict';

    load('jstests/libs/feature_compatibility_version.js');

    // Create a data directory using a 3.4 binary.
    let conn = MongoRunner.runMongod({binVersion: '3.4'});
    const dbpath = conn.dbpath;
    MongoRunner.stopMongod(conn);

    // Start mongod using the 4.0 binary on the 3.4 data directory and attempt to repair.
    MongoRunner.runMongod({binVersion: 'latest', dbpath: dbpath, noCleanData: true, repair: ''});

    // Starting mongod with a 3.4 binary after the repair should still work.
    conn = MongoRunner.runMongod({binVersion: '3.4', dbpath: dbpath, noCleanData: true});
    assert.neq(null, conn, 'mongod was unable to start up after repairing');
    MongoRunner.stopMongod(conn);

    // Ensure that the data files are incompatible with the 4.0 binary of mongod and that it exits
    // cleanly.
    let returnCode = runMongoProgram("mongod", "--port", conn.port, "--dbpath", dbpath);
    assert.eq(62, returnCode);
}());
