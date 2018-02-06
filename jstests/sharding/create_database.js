/**
 * Tests that creating a database causes it to be written to the sharding catalog with a
 * databaseVersion if FCV > 3.6, but not if FCV <= 3.6.
 */
(function() {
    'use strict';

    function createDatabase(st, dbName) {
        // A database is implicitly created when a collection inside it is created.
        assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
    }

    function cleanUp(st, dbName) {
        assert.commandWorked(st.s.getDB(dbName).runCommand({dropDatabase: 1}));
    }

    function assertHasDbVersion(dbEntry) {
        assert.neq(null, dbEntry);
        assert.neq(null, dbEntry.version);
        assert.neq(null, dbEntry.version.uuid);
        assert.eq(1, dbEntry.version.lastMod);
    }

    function assertDoesntHaveDbVersion(dbEntry) {
        assert.neq(null, dbEntry);
        assert.eq(null, dbEntry.version);
    }

    const dbName = "db1";
    const collName = "foo";

    var st = new ShardingTest({shards: 1});

    let dbEntry;

    //
    // FCV 4.0
    //

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    // A new database is given a databaseVersion.
    createDatabase(st, dbName);
    let dbEntry1 = st.s.getDB("config").getCollection("databases").findOne({_id: dbName});
    assertHasDbVersion(dbEntry1);
    cleanUp(st, dbName);

    // A new incarnation of a database that was previously dropped is given a fresh databaseVersion.
    createDatabase(st, dbName);
    let dbEntry2 = st.s.getDB("config").getCollection("databases").findOne({_id: dbName});
    assertHasDbVersion(dbEntry2);
    assert.neq(dbEntry1.version.uuid, dbEntry2.version.uuid);
    cleanUp(st, dbName);

    //
    // FCV 3.6
    //

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    // A new database is not given a databaseVersion.
    createDatabase(st, dbName);
    dbEntry = st.s.getDB("config").getCollection("databases").findOne({_id: dbName});
    assertDoesntHaveDbVersion(dbEntry);
    cleanUp(st, dbName);

    //
    // FCV 3.4 (This section can be deleted once FCV 3.4 is removed).
    //

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // A new database is not given a databaseVersion.
    createDatabase(st, dbName);
    dbEntry = st.s.getDB("config").getCollection("databases").findOne({_id: dbName});
    assertDoesntHaveDbVersion(dbEntry);
    cleanUp(st, dbName);

})();
