/**
 * Tests that after a shard refreshes its cached database entry, the new database
 * entry is persisted to disk.
 */
(function() {
    "use strict";

    const st = new ShardingTest(
        {rs0: {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0, votes: 0}}]}});

    const db = "test";
    const coll = "bar";
    const nss = db + "." + coll;

    assert.commandWorked(st.s.adminCommand({enableSharding: db}));
    st.ensurePrimaryShard(db, st.shard0.shardName);

    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'callShardServerCallbackFn', mode: 'alwaysOn'}));

    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

    // Run listCollections because it attaches databaseVersion, so it forces the shard to load
    // the routing info for the database if it has not already
    assert.commandWorked(st.s.getDB(db).runCommand({listCollections: 1, filter: {name: nss}}));

    assert.commandWorked(
        st.rs0.getPrimary().getDB('admin').runCommand({_flushDatabaseCacheUpdates: db}));
    st.rs0.awaitReplication();

    // Check that the db version is persisted on the primary shard.
    const cacheDbEntry = assert.commandWorked(st.shard0.getDB("config").runCommand({
        find: "cache.databases",
        filter: {_id: db},
        $readPreference: {mode: 'primary'},
        readConcern: {'level': 'local'}
    }));
    const primaryRes = cacheDbEntry.cursor.firstBatch[0];
    assert.neq(undefined, primaryRes);
    assert.neq(undefined, primaryRes.version);
    assert.neq(undefined, primaryRes.version.uuid);
    assert.neq(undefined, primaryRes.version.lastMod);
    assert.neq(undefined, primaryRes.primary);
    assert.neq(undefined, primaryRes.partitioned);

    // Check that the db version is persisted on the secondary shard.
    const cacheDbEntrySecondary = assert.commandWorked(st.shard0.getDB("config").runCommand({
        find: "cache.databases",
        filter: {_id: db},
        $readPreference: {mode: 'secondary'},
        readConcern: {'level': 'local'}
    }));
    const secondaryRes = cacheDbEntrySecondary.cursor.firstBatch[0];

    // Assert that the database entries on the primary and secondary are the same.
    assert.docEq(primaryRes,
                 secondaryRes,
                 "Primary and secondary nodes have different entries for " + db +
                     " in config.cache.databases.");

    st.stop();
})();
