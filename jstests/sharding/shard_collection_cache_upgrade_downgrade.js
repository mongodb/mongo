/**
 * Tests that 'config.cache.collections' on shards will get updated with UUIDs the first time
 * the shard receives a versioned request for the collection after setFCV=4.0.
 */
(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    function checkCachedCollectionEntry(conn, ns, authoritativeEntry) {
        const res = conn.getDB("config").runCommand({find: "cache.collections", filter: {_id: ns}});
        assert.commandWorked(res);
        const cacheEntry = res.cursor.firstBatch[0];
        if (authoritativeEntry === undefined) {
            assert.eq(undefined, cacheEntry);
        } else {
            assert.eq(cacheEntry.uuid,
                      authoritativeEntry.uuid,
                      conn + " did not have expected on-disk cached collection UUID for " + ns);
        }
    }

    function checkCachedChunksEntry(conn, ns, authoritativeEntry) {
        const res = conn.getDB("config").runCommand({find: "cache.chunks." + ns});
        assert.commandWorked(res);
        const cacheEntry = res.cursor.firstBatch[0];
        if (authoritativeEntry === undefined) {
            assert.eq(undefined, cacheEntry);
        } else {
            if (authoritativeEntry.hasOwnProperty("history")) {
                assert(cacheEntry.hasOwnProperty("history"),
                       conn + "did not have expected on-disk cached history for " + ns);
            } else {
                assert(!cacheEntry.hasOwnProperty("history"),
                       conn + "did have unexpected on-disk cached history for " + ns);
            }
        }
    }
    const st = new ShardingTest({shards: 2});
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    const db1Name = "db1";
    const db2Name = "db2";
    const collName = "foo";
    const ns1 = db1Name + "." + collName;
    const ns2 = db2Name + "." + collName;

    // Create both collections in the sharding catalog and ensure they are on different shards.
    assert.commandWorked(st.s.adminCommand({enableSharding: db1Name}));
    assert.commandWorked(st.s.adminCommand({enableSharding: db2Name}));
    st.ensurePrimaryShard(db1Name, st.shard0.shardName);
    st.ensurePrimaryShard(db2Name, st.shard1.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns1, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns2, key: {_id: 1}}));

    // Ensure the collection entries have UUIDs.
    const ns1EntryOriginal = st.s.getDB("config").getCollection("collections").findOne({_id: ns1});
    const ns2EntryOriginal = st.s.getDB("config").getCollection("collections").findOne({_id: ns2});
    assert.neq(null, ns1EntryOriginal.uuid);
    assert.neq(null, ns2EntryOriginal.uuid);

    const ns1ChunkEntryOriginal = st.s.getDB("config").getCollection("chunks").findOne({ns: ns1});
    const ns2ChunkEntryOriginal = st.s.getDB("config").getCollection("chunks").findOne({ns: ns2});
    assert.neq(null, ns1ChunkEntryOriginal);
    assert(!ns1ChunkEntryOriginal.hasOwnProperty("history"));
    assert.neq(null, ns2ChunkEntryOriginal);
    assert(!ns2ChunkEntryOriginal.hasOwnProperty("history"));

    // Force each shard to refresh for the collection it owns to ensure it writes a cache entry.

    assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns1}));
    assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns2}));

    checkCachedCollectionEntry(st.shard0, ns1, ns1EntryOriginal);
    checkCachedCollectionEntry(st.shard0, ns2, undefined);
    checkCachedCollectionEntry(st.shard1, ns1, undefined);
    checkCachedCollectionEntry(st.shard1, ns2, ns2EntryOriginal);

    checkCachedChunksEntry(st.shard0, ns1, ns1ChunkEntryOriginal);
    checkCachedChunksEntry(st.shard0, ns2, undefined);
    checkCachedChunksEntry(st.shard1, ns1, undefined);
    checkCachedChunksEntry(st.shard1, ns2, ns2ChunkEntryOriginal);

    // Simulate that the cache entry was written without a UUID (SERVER-33356).
    assert.writeOK(st.shard0.getDB("config")
                       .getCollection("cache.collections")
                       .update({}, {$unset: {uuid: ""}}, {multi: true}));
    assert.writeOK(st.shard1.getDB("config")
                       .getCollection("cache.collections")
                       .update({}, {$unset: {uuid: ""}}, {multi: true}));

    //
    // setFCV 4.0 (upgrade)
    //

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // The UUID in the authoritative collection entries should not have changed.
    const ns1EntryFCV40 = st.s.getDB("config").getCollection("collections").findOne({_id: ns1});
    const ns2EntryFCV40 = st.s.getDB("config").getCollection("collections").findOne({_id: ns2});
    assert.docEq(ns1EntryFCV40, ns1EntryOriginal);
    assert.docEq(ns2EntryFCV40, ns2EntryOriginal);

    const ns1ChunkEntryFCV40 = st.s.getDB("config").getCollection("chunks").findOne({ns: ns1});
    const ns2ChunkEntryFCV40 = st.s.getDB("config").getCollection("chunks").findOne({ns: ns2});
    assert.neq(null, ns1ChunkEntryFCV40);
    assert(ns1ChunkEntryFCV40.hasOwnProperty("history"));
    assert.neq(null, ns2ChunkEntryFCV40);
    assert(ns2ChunkEntryFCV40.hasOwnProperty("history"));

    st.s.getDB(db1Name).getCollection(collName).findOne();
    st.s.getDB(db2Name).getCollection(collName).findOne();

    // We wait for the refresh triggered by the finds to persist the new cache entry to disk,
    // because it's done asynchronously.
    assert.commandWorked(
        st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns1, syncFromConfig: false}));
    assert.commandWorked(
        st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns2, syncFromConfig: false}));

    // The shards' collection caches should have been updated with UUIDs.
    checkCachedCollectionEntry(st.shard0, ns1, ns1EntryOriginal);
    checkCachedCollectionEntry(st.shard0, ns2, undefined);
    checkCachedCollectionEntry(st.shard1, ns1, undefined);
    checkCachedCollectionEntry(st.shard1, ns2, ns2EntryOriginal);

    // The shards' chunk caches should have been updated with histories.
    checkCachedChunksEntry(st.shard0, ns1, ns1ChunkEntryFCV40);
    checkCachedChunksEntry(st.shard0, ns2, undefined);
    checkCachedChunksEntry(st.shard1, ns1, undefined);
    checkCachedChunksEntry(st.shard1, ns2, ns2ChunkEntryFCV40);

    //
    // setFCV 3.6 (downgrade)
    //

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    // The UUID in the authoritative collection entries should still not have changed.
    const ns1EntryFCV36 = st.s.getDB("config").getCollection("collections").findOne({_id: ns1});
    const ns2EntryFCV36 = st.s.getDB("config").getCollection("collections").findOne({_id: ns2});
    assert.docEq(ns1EntryFCV36, ns1EntryOriginal);
    assert.docEq(ns2EntryFCV36, ns2EntryOriginal);

    const ns1ChunkEntryFCV36 = st.s.getDB("config").getCollection("chunks").findOne({ns: ns1});
    const ns2ChunkEntryFCV36 = st.s.getDB("config").getCollection("chunks").findOne({ns: ns2});
    assert.neq(null, ns1ChunkEntryFCV36);
    assert(!ns1ChunkEntryFCV36.hasOwnProperty("history"));
    assert.neq(null, ns2ChunkEntryFCV36);
    assert(!ns2ChunkEntryFCV36.hasOwnProperty("history"));

    st.s.getDB(db1Name).getCollection(collName).findOne();
    st.s.getDB(db2Name).getCollection(collName).findOne();

    assert.commandWorked(
        st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns1, syncFromConfig: false}));
    assert.commandWorked(
        st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns2, syncFromConfig: false}));

    // Also refresh the sessions collection so that the UUID consistency check at the end of
    // ShardingTest, which will check for its UUID on the shards, passes.
    assert.commandWorked(
        st.shard0.adminCommand({_flushRoutingTableCacheUpdates: "config.system.sessions"}));
    assert.commandWorked(
        st.shard1.adminCommand({_flushRoutingTableCacheUpdates: "config.system.sessions"}));

    // The shards' collection caches should not have changed.
    checkCachedCollectionEntry(st.shard0, ns1, ns1EntryOriginal);
    checkCachedCollectionEntry(st.shard0, ns2, undefined);
    checkCachedCollectionEntry(st.shard1, ns1, undefined);
    checkCachedCollectionEntry(st.shard1, ns2, ns2EntryOriginal);

    // The shards' chunk caches should have been updated with histories removed.
    checkCachedChunksEntry(st.shard0, ns1, ns1ChunkEntryFCV40);
    checkCachedChunksEntry(st.shard0, ns2, undefined);
    checkCachedChunksEntry(st.shard1, ns1, undefined);
    checkCachedChunksEntry(st.shard1, ns2, ns2ChunkEntryFCV40);

    st.stop();
})();
