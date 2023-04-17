/**
 * Tests that shard removal triggers an update of the catalog cache so that routers don't continue
 * to target shards that have been removed.
 */
(function() {
'use strict';
load('jstests/sharding/libs/remove_shard_util.js');

// Checking UUID consistency involves talking to shards, but this test shuts down shards.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const dbName = 'TestDB';

/**
 * Test that sharded collections with data on a shard that gets removed are correctly invalidated in
 * a router's catalog cache.
 *
 * 1. Create 2 shards and 2 routers. Make shard0 the primary shard for a database.
 * 2. Put data for a sharded collection on shard0.
 * 3. Ensure both routers have up-to-date routing info.
 * 4. Remove shard0 by sending removeShard through router 0. All data will be migrated to shard1.
 * 5. Send a query through router 1 to target the sharded collection. This should correctly target
 *    shard1.
 */
(() => {
    jsTestLog(
        "Test that sharded collections with data on a shard that gets removed are correctly invalidated in a router's catalog cache.");

    const shardedCollName = 'Coll';
    const shardedCollNs = dbName + '.' + shardedCollName;

    var st = new ShardingTest({shards: 2, mongos: 2});
    let router0ShardedColl = st.s0.getDB(dbName)[shardedCollName];
    let router1ShardedColl = st.s1.getDB(dbName)[shardedCollName];

    assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard1.shardName);
    assert.commandWorked(st.s0.adminCommand({shardCollection: shardedCollNs, key: {_id: 1}}));

    // Make sure data is inserted into shard0
    assert.commandWorked(st.s0.adminCommand({
        moveChunk: shardedCollNs,
        find: {_id: -1},
        to: st.shard0.shardName,
        _waitForDelete: true
    }));

    // Insert some documents into the sharded collection on shard0.
    router0ShardedColl.insert({_id: -1});
    router0ShardedColl.insert({_id: 1});

    // Force s0 and s1 to load the database and collection cache entries for the sharded collection.
    assert.eq(2, router0ShardedColl.find({}).itcount());
    assert.eq(2, router1ShardedColl.find({}).itcount());

    // Start the balancer here so that it can drain shard0 when it's removed but also won't conflict
    // with the above moveChunk command.
    st.startBalancer();

    // Remove shard0.
    removeShard(st, st.shard0.shardName);

    // Stop the replica set so that future requests to this shard will be unsuccessful. Skip this
    // step for a config shard, since the config server must be up for the second router to
    // refresh. The default read concern is local, so the router should eventually target a shard
    // with chunks.
    if (!TestData.configShard) {
        st.rs0.stopSet();
    }

    // Ensure that s1, the router which did not run removeShard, eventually stops targeting chunks
    // for the sharded collection which previously resided on a shard that no longer exists.
    assert.soon(() => {
        try {
            const count = router1ShardedColl.count({_id: 1});
            return true;
        } catch (e) {
            print(e);
            return false;
        }
    });

    st.stop();
})();

/**
 * Test that entries for a database whose original primary shard gets removed are correctly
 * invalidated in a router's catalog cache.
 *
 * 1. Create 2 shards and 2 routers. Make shard0 the primary shard for a database.
 * 2. Put data for an unsharded collection on shard0.
 * 3. Ensure both routers have up-to-date routing info.
 * 4. movePrimary for the database to shard1.
 * 4. Remove shard0 by sending removeShard through router 0.
 * 5. Send a query through router 1 to target the sharded and unsharded collections. This should
 *    correctly target shard1.
 */
(() => {
    jsTestLog(
        "Test that entries for a database whose original primary shard gets removed are correctly invalidated in a router's catalog cache.");

    const unshardedCollName = 'UnshardedColl';
    const unshardedCollNs = dbName + '.' + unshardedCollName;

    var st = new ShardingTest({shards: 2, mongos: 2, other: {enableBalancer: true}});

    let router0UnshardedColl = st.s0.getDB(dbName)[unshardedCollName];
    let router1UnshardedColl = st.s1.getDB(dbName)[unshardedCollName];

    assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    // Insert some documents into the unsharded collection whose primary is the to-be-removed
    // shard0.
    router0UnshardedColl.insert({_id: 1});

    // Force s0 and s1 to load the database and collection cache entries for the unsharded
    // collection.
    assert.eq(1, router0UnshardedColl.find({}).itcount());
    assert.eq(1, router1UnshardedColl.find({}).itcount());

    // Call movePrimary for the database so that shard0 can be removed.
    assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

    // Remove shard0. We need assert.soon since chunks in the sessions collection may need to be
    // migrated off by the balancer.
    removeShard(st, st.shard0.shardName);

    // Stop the replica set so that future requests to this shard will be unsuccessful. Skip this
    // step for a config shard, since the config server must be up for the second router to
    // refresh. The default read concern is local, so the router should eventually target a shard
    // with chunks.
    if (!TestData.configShard) {
        st.rs0.stopSet();
    }

    // Ensure that s1, the router which did not run removeShard, eventually stops targeting data for
    // the unsharded collection which previously had as primary a shard that no longer exists.
    assert.soon(() => {
        try {
            const count = router1UnshardedColl.count({_id: 1});
            return true;
        } catch (e) {
            print(e);
            return false;
        }
    });
    st.stop();
})();
})();
