/**
 * Tests that
 * 1) the 'config.databases' schema gets upgraded/downgraded on setFCV
 * 2) shards' in-memory cached database versions get cleared on FCV downgrade (on both the primary
      and secondary nodes)
 * 3) shards only cache the database version in-memory in FCV 4.0.
 */
(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/database_versioning.js");

    const st = new ShardingTest(
        {shards: {rs0: {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0, votes: 0}}]}}});
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    st.rs0.awaitReplication();  // Ensure secondary reaches the new FCV.

    const db1Name = "db1";
    const db2Name = "db2";
    const collName = "foo";
    const ns = db1Name + "." + collName;

    // Create both databases in the sharding catalog.
    assert.commandWorked(st.s.adminCommand({enableSharding: db1Name}));
    assert.commandWorked(st.s.adminCommand({enableSharding: db2Name}));

    // Ensure neither database entry has a database version.
    const db1EntryOriginal =
        st.s.getDB("config").getCollection("databases").findOne({_id: db1Name});
    const db2EntryOriginal =
        st.s.getDB("config").getCollection("databases").findOne({_id: db2Name});
    assert.eq(null, db1EntryOriginal.version);
    assert.eq(null, db2EntryOriginal.version);

    // Ensure the shard does not have cached entries in-memory or on-disk.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db1Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db1Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db2Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db2Name, {});
    checkOnDiskDatabaseVersion(st.shard0, db1Name, undefined);
    checkOnDiskDatabaseVersion(st.shard0, db2Name, undefined);

    // Create a collection in 'db1' to ensure 'db1' gets created on the primary shard.
    // TODO (SERVER-34431): Remove this once the DatabaseShardingState is in a standalone map.
    assert.commandWorked(st.s.getDB(db1Name).runCommand({create: collName}));

    // Force the shard database to refresh and ensure it neither writes a cache entry, nor does it
    // cache the version in memory
    assert.commandWorked(st.rs0.getPrimary().adminCommand({_flushDatabaseCacheUpdates: db1Name}));
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db1Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db1Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db2Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db2Name, {});
    checkOnDiskDatabaseVersion(st.shard0, db1Name, undefined);
    checkOnDiskDatabaseVersion(st.shard0, db2Name, undefined);

    //
    // setFCV 4.0 (upgrade)
    //

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    st.rs0.awaitReplication();  // Ensure secondary reaches the new FCV.

    // Database versions should have been generated for the authoritative database entries.
    const db1EntryFCV40 = st.s.getDB("config").getCollection("databases").findOne({_id: db1Name});
    const db2EntryFCV40 = st.s.getDB("config").getCollection("databases").findOne({_id: db2Name});
    assert.neq(null, db1EntryFCV40.version);
    assert.neq(null, db2EntryFCV40.version);

    // Before the shard receives a versioned request, its in-memory and on-disk caches should not
    // have been refreshed.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db1Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db1Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db2Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db2Name, {});
    checkOnDiskDatabaseVersion(st.shard0, db1Name, undefined);
    checkOnDiskDatabaseVersion(st.shard0, db2Name, undefined);

    // After receiving a versioned request for a database, the shard should refresh its in-memory
    // and on-disk caches for that database.

    // This is needed because mongos will not automatically refresh its database entry after the
    // cluster's FCV changes to pick up the database version (see SERVER-34460).
    assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));

    assert.commandWorked(st.s.getDB(db1Name).runCommand(
        {listCollections: 1, $readPreference: {mode: "secondary"}, readConcern: {level: "local"}}));

    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db1Name, db1EntryFCV40.version);
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db1Name, db1EntryFCV40.version);
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db2Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db2Name, {});
    checkOnDiskDatabaseVersion(st.shard0, db1Name, db1EntryFCV40);
    checkOnDiskDatabaseVersion(st.shard0, db2Name, undefined);

    //
    // setFCV 3.6 (downgrade)
    //

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    st.rs0.awaitReplication();  // Ensure secondary reaches the new FCV.

    // Database versions should have been removed from the authoritative database entries.
    const db1EntryFCV36 = st.s.getDB("config").getCollection("databases").findOne({_id: db1Name});
    const db2EntryFCV36 = st.s.getDB("config").getCollection("databases").findOne({_id: db2Name});
    assert.docEq(db1EntryOriginal, db1EntryFCV36);
    assert.docEq(db2EntryOriginal, db2EntryFCV36);

    // The shard's in-memory and on-disk database caches should have been left untouched.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db1Name, db1EntryFCV40.version);
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db1Name, db1EntryFCV40.version);
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), db2Name, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), db2Name, {});
    checkOnDiskDatabaseVersion(st.shard0, db1Name, db1EntryFCV40);
    checkOnDiskDatabaseVersion(st.shard0, db2Name, undefined);

    st.stop();
})();
