// This ensures that if a shard had cached a database entry without a version in a 4.0 cluster while
// in FCV 3.6, then upgrading the cluster to 4.2 binaries will cause the next request that has a
// database version attached to cause the shard to refresh and pick up the version and write it to
// its on-disk cache so that the version can also be picked up by secondaries.
(function() {

    const st = new ShardingTest({shards: 1, rs: {nodes: 2}, other: {verbose: 2}});

    assert.commandWorked(st.s.getDB("test").getCollection("foo").insert({x: 1}));

    // The database is created with a version.
    const versionOnConfig =
        st.s.getDB("config").getCollection("databases").findOne({_id: "test"}).version;
    assert.neq(null, versionOnConfig);

    // Before the shard refreshes, it does not have a cache entry for the database.
    assert.eq(null,
              st.shard0.getDB("config").getCollection("cache.databases").findOne({_id: "test"}));

    // After the shard refreshes, it has a cache entry for the database with version matching the
    // version on the config server.
    assert.commandWorked(st.shard0.adminCommand({_flushDatabaseCacheUpdates: "test"}));
    const versionOnShard =
        st.shard0.getDB("config").getCollection("cache.databases").findOne({_id: "test"}).version;
    assert.docEq(versionOnConfig, versionOnShard);

    // The shard primary's in-memory version matches the on-disk version.
    assert.eq(versionOnShard, st.shard0.adminCommand({getDatabaseVersion: "test"}).dbVersion);

    jsTest.log("Remove the database version from the shard's cache entry");
    assert.commandWorked(
        st.shard0.getDB("config").getCollection("cache.databases").update({_id: "test"}, {
            $unset: {version: ""}
        }));
    assert.eq(
        null,
        st.shard0.getDB("config").getCollection("cache.databases").findOne({_id: "test"}).version);

    // Deleting the version field from the on-disk entry did not affect the in-memory version.
    assert.eq(versionOnShard, st.shard0.adminCommand({getDatabaseVersion: "test"}).dbVersion);

    // The shard secondary does not have a version cached in memory.
    assert.eq({}, st.rs0.getSecondary().adminCommand({getDatabaseVersion: "test"}).dbVersion);

    // A versioned request against the shard secondary makes the shard primary refresh and update
    // the on-disk cache entry with a version, even though it already had an on-disk cache entry and
    // had the up-to-date version cached in memory.
    // Use readConcern 'local' because the default on secondaries is 'available'.
    assert.commandWorked(st.s.getDB("test").runCommand(
        {listCollections: 1, $readPreference: {mode: "secondary"}, readConcern: {level: "local"}}));
    const versionOnShard2 =
        st.shard0.getDB("config").getCollection("cache.databases").findOne({_id: "test"}).version;
    assert.docEq(versionOnConfig, versionOnShard2);

    // The shard secondary's in-memory version now matches the on-disk version.
    assert.eq(versionOnShard,
              st.rs0.getSecondary().adminCommand({getDatabaseVersion: "test"}).dbVersion);

    st.stop();

})();
