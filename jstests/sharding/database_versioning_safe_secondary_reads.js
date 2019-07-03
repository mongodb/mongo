/**
 * Tests that shards' cached in-memory and on-disk database versions are updated on primary and
 * secondary nodes when:
 * - the node does not have a cached in-memory version
 * - the node's cached in-memory version is lower than the version sent by a client
 * - the movePrimary critical section is entered on the primary node
 */
(function() {
    "use strict";
    load("jstests/libs/database_versioning.js");

    const dbName = "test";

    const st = new ShardingTest({
        mongos: 2,
        rs0: {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0}}]},
        rs1: {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0}}]},
        verbose: 2
    });

    // Before creating the database, none of the nodes have a cached entry for the database either
    // in memory or on disk.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, undefined);

    // Use 'enableSharding' to create the database only in the sharding catalog (the database will
    // not exist on any shards). Manually update the 'primary' field in the new dbEntry in
    // config.databases so that we know which shard is the primary.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.getDB("config").getCollection("databases").update({_id: dbName}, {
        $set: {primary: st.rs1.name}
    }));

    // Check that a command that attaches databaseVersion returns empty results, even though the
    // database does not actually exist on any shard.
    assert.commandWorked(st.s.getDB(dbName).runCommand({listCollections: 1}));

    // Ensure the current primary shard's primary has written the new database entry to disk.
    st.rs1.getPrimary().adminCommand({_flushDatabaseCacheUpdates: dbName, syncFromConfig: false});

    // Ensure the database entry on the current primary shard has replicated to the secondary.
    st.rs1.awaitReplication();

    const dbEntry0 = st.s.getDB("config").getCollection("databases").findOne({_id: dbName});

    // The primary shard's primary should have refreshed its in-memory and on-disk caches. The
    // primary shard's secondary should have refreshed its on-disk entry.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry0.version);
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry0);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry0);

    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {listCollections: 1, $readPreference: {mode: "secondary"}, readConcern: {level: "local"}}));

    // The primary shard's secondary should have refreshed its in-memory cache (its on-disk cache
    // was updated when the primary refreshed, above).
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry0.version);
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry0.version);
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry0);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry0);

    // Use 'movePrimary' to ensure shard0 is the primary shard.
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    const dbEntry1 = st.s.getDB("config").getCollection("databases").findOne({_id: dbName});
    assert.eq(dbEntry1.version.uuid, dbEntry0.version.uuid);
    assert.eq(dbEntry1.version.lastMod, dbEntry0.version.lastMod + 1);

    // Ensure the database actually gets created on the primary shard by creating a collection in
    // it.
    assert.commandWorked(st.s.getDB(dbName).runCommand({create: "foo"}));

    // Run a command that attaches databaseVersion to cause the current primary shard's primary to
    // refresh its in-memory cached database version.
    jsTest.log("About to do listCollections with readPref=primary");
    assert.commandWorked(st.s.getDB(dbName).runCommand({listCollections: 1}));

    // Ensure the current primary shard's primary has written the new database entry to disk.
    st.rs0.getPrimary().adminCommand({_flushDatabaseCacheUpdates: dbName, syncFromConfig: false});

    // Ensure the database entry on the current primary shard has replicated to the secondary.
    st.rs0.awaitReplication();

    // The primary shard's primary should have refreshed its in-memory cache and on-disk entry. The
    // primary shard's secondary will have refreshed its on-disk entry.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry1.version);
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry1);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry0);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry1);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry0);

    // Now run a command that attaches databaseVersion with readPref=secondary to make the current
    // primary shard's secondary refresh its in-memory database version from its on-disk entry.
    jsTest.log("About to do listCollections with readPref=secondary");
    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {listCollections: 1, $readPreference: {mode: "secondary"}, readConcern: {level: "local"}}));

    // The primary shard's secondary should have refreshed its in memory-cache.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry1.version);
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry1.version);
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry1);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry0);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry1);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry0);

    // Make "staleMongos" load the stale database info into memory.
    const freshMongos = st.s0;
    const staleMongos = st.s1;
    staleMongos.getDB(dbName).runCommand({listCollections: 1});

    // Run movePrimary to ensure the movePrimary critical section clears the in-memory cache on the
    // old primary shard.
    jsTest.log("About to do movePrimary");
    assert.commandWorked(freshMongos.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));
    const dbEntry2 = freshMongos.getDB("config").getCollection("databases").findOne({_id: dbName});
    assert.eq(dbEntry2.version.uuid, dbEntry1.version.uuid);
    assert.eq(dbEntry2.version.lastMod, dbEntry1.version.lastMod + 1);

    // Ensure the old primary shard's primary has written the 'enterCriticalSectionSignal' flag to
    // its on-disk database entry.
    st.rs0.getPrimary().adminCommand({_flushDatabaseCacheUpdates: dbName, syncFromConfig: false});

    // Ensure 'enterCriticalSectionSignal' flag has replicated to the secondary.
    st.rs0.awaitReplication();

    // The in-memory cached version should have been cleared on the old primary shard's primary and
    // secondary nodes.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry1);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry0);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry1);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry0);

    // Run listCollections with readPref=secondary from the stale mongos. First, this should cause
    // the old primary shard's secondary to provoke the old primary shard's primary to refresh. Then
    // once the stale mongos refreshes, it should cause the new primary shard's secondary to provoke
    // the new primary shard's primary to refresh.
    jsTest.log("About to do listCollections with readPref=secondary after movePrimary");
    assert.commandWorked(staleMongos.getDB(dbName).runCommand(
        {listCollections: 1, $readPreference: {mode: "secondary"}, readConcern: {level: "local"}}));

    // All nodes should have refreshed.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry2.version);
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry2.version);
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry2.version);
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry2.version);
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry2);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, dbEntry2);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry2);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, dbEntry2);

    // Drop the database
    jsTest.log("About to drop database from the cluster");
    assert.commandWorked(freshMongos.getDB(dbName).runCommand({dropDatabase: 1}));

    // Ensure the drop has replicated to all nodes.
    st.rs0.awaitReplication();
    st.rs1.awaitReplication();

    // The in-memory caches will be cleared on both shard's primary nodes and the on-disk database
    // entries will be cleared on all shards.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, undefined);

    // Use 'enableSharding' to create the database only in the sharding catalog (the database will
    // not exist on any shards).
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

    // Simulate that the database was created on 'shard0' by directly modifying the database entry
    // (we cannot use movePrimary, since movePrimary creates the database on the shards).
    assert.writeOK(st.s.getDB("config").getCollection("databases").update({_id: dbName}, {
        $set: {primary: st.shard0.shardName}
    }));
    const dbEntry = st.s.getDB("config").getCollection("databases").findOne({_id: dbName});

    assert.commandWorked(st.s.getDB(dbName).runCommand({listCollections: 1}));

    // Ensure the current primary shard's primary has written the new database entry to disk.
    st.rs0.getPrimary().adminCommand({_flushDatabaseCacheUpdates: dbName, syncFromConfig: false});

    // Ensure the database entry on the current primary shard has replicated to the secondary.
    st.rs0.awaitReplication();

    // The primary shard's primary should have refreshed its in-memory and on-disk caches.
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry.version);
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, undefined);

    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {listCollections: 1, $readPreference: {mode: "secondary"}, readConcern: {level: "local"}}));

    // The primary shard's secondary should have refreshed its in-memory cache (its on-disk cache
    // was already updated when the primary refreshed).
    checkInMemoryDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry.version);
    checkInMemoryDatabaseVersion(st.rs1.getPrimary(), dbName, {});
    checkInMemoryDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry.version);
    checkInMemoryDatabaseVersion(st.rs1.getSecondary(), dbName, {});
    checkOnDiskDatabaseVersion(st.rs0.getPrimary(), dbName, dbEntry);
    checkOnDiskDatabaseVersion(st.rs1.getPrimary(), dbName, undefined);
    checkOnDiskDatabaseVersion(st.rs0.getSecondary(), dbName, dbEntry);
    checkOnDiskDatabaseVersion(st.rs1.getSecondary(), dbName, undefined);

    st.stop();
})();
