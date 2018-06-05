/**
 * Tests that creating a database causes it to be written to the sharding catalog with a
 * databaseVersion if FCV > 3.6, but not if FCV <= 3.6.
 */
(function() {
    'use strict';

    function createDatabase(mongos, dbName) {
        // A database is implicitly created when a collection inside it is created.
        assert.commandWorked(mongos.getDB(dbName).runCommand({create: collName}));
    }

    function cleanUp(mongos, dbName) {
        assert.commandWorked(mongos.getDB(dbName).runCommand({dropDatabase: 1}));
    }

    function assertDbVersionAssigned(mongos, dbName) {
        createDatabase(mongos, dbName);

        // Check that the entry in the sharding catalog contains a dbVersion.
        const dbEntry = mongos.getDB("config").getCollection("databases").findOne({_id: dbName});
        assert.neq(null, dbEntry);
        assert.neq(null, dbEntry.version);
        assert.neq(null, dbEntry.version.uuid);
        assert.eq(1, dbEntry.version.lastMod);

        // Check that the catalog cache on the mongos contains the same dbVersion.
        const cachedDbEntry = mongos.adminCommand({getShardVersion: dbName});
        assert.commandWorked(cachedDbEntry);
        assert.eq(dbEntry.version.uuid, cachedDbEntry.version.uuid);
        assert.eq(dbEntry.version.lastMod, cachedDbEntry.version.lastMod);

        cleanUp(mongos, dbName);

        return dbEntry;
    }

    function assertDbVersionNotAssigned(mongos, dbName) {
        createDatabase(mongos, dbName);

        // Check that the entry in the sharding catalog *does not* contain a dbVersion.
        const dbEntry = mongos.getDB("config").getCollection("databases").findOne({_id: dbName});
        assert.neq(null, dbEntry);
        assert.eq(null, dbEntry.version);

        // Check that the catalog cache on the mongos *does not* contain a dbVersion.
        const cachedDbEntry = mongos.adminCommand({getShardVersion: dbName});
        assert.commandWorked(cachedDbEntry);
        assert.eq(null, cachedDbEntry.version);

        cleanUp(mongos, dbName);

        return dbEntry;
    }

    const dbName = "db1";
    const collName = "foo";
    const ns = dbName + "." + collName;

    var st = new ShardingTest({shards: 1});

    // A new database is given a databaseVersion.
    let dbEntry1 = assertDbVersionAssigned(st.s, dbName);

    // A new incarnation of a database that was previously dropped is given a fresh databaseVersion.
    let dbEntry2 = assertDbVersionAssigned(st.s, dbName);
    assert.neq(dbEntry1.version.uuid, dbEntry2.version.uuid);

    st.stop();

})();
