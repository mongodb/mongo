/**
 * Tests that write operations on the collection being resharded succeed even when the routing
 * information for the associated temporary resharding collection is stale.
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/libs/fail_point_util.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({reshardInPlace: false});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    primaryShardName: recipientShardNames[0],
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor = new Mongo(topology.shards[donorShardNames[0]].primary);

const reshardingPauseDonorBeforeCatalogCacheRefreshFailpoint =
    configureFailPoint(donor, "reshardingPauseDonorBeforeCatalogCacheRefresh");

// We trigger a refresh to make the catalog cache track the routing info for the temporary
// resharding namespace as unsharded because the collection won't exist yet.
assert.commandWorked(donor.adminCommand({_flushRoutingTableCacheUpdates: reshardingTest.tempNs}));

reshardingTest.withReshardingInBackground(  //
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        reshardingPauseDonorBeforeCatalogCacheRefreshFailpoint.wait();

        assert.commandWorked(sourceCollection.insert({_id: 0, oldKey: 5, newKey: 15}));
        reshardingPauseDonorBeforeCatalogCacheRefreshFailpoint.off();
    });

reshardingTest.teardown();
})();
