/**
 * Tests that after a shard refreshes its cached database entry, the new database
 * entry is persisted to disk.
 */
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2});

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

    // Check that the db version is persisted on the shard.
    const cacheDbEntry = st.shard0.getDB("config").cache.databases.findOne({_id: db});
    assert.neq(undefined, cacheDbEntry);
    assert.neq(undefined, cacheDbEntry.version);
    assert.neq(undefined, cacheDbEntry.version.uuid);
    assert.neq(undefined, cacheDbEntry.version.lastMod);
    assert.neq(undefined, cacheDbEntry.primary);
    assert.neq(undefined, cacheDbEntry.partitioned);

    st.stop();
})();
