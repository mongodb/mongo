/**
 * Tests that replica set shard secondaries don't attempt to create primary-only service Instances
 * or insert state documents for resharding.
 *
 * @tags: [
 *   uses_atclustertime
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();

reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

function triggerShardVersionRefreshOnSecondary(collection) {
    // The shard version refresh would have failed and prevented the secondary from being read from
    // if it had attempted to create a primary-only service Instance or insert a state document for
    // resharding.
    assert.commandWorked(mongos.adminCommand({flushRouterConfig: collection.getFullName()}));
    collection.find().readPref("secondary").readConcern("local").itcount();
}

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const configPrimary = new Mongo(topology.configsvr.primary);
const fp = configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeCloning");

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    //
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    (tempNs) => {
        fp.wait();

        triggerShardVersionRefreshOnSecondary(sourceCollection);

        const tempColl = mongos.getCollection(tempNs);
        triggerShardVersionRefreshOnSecondary(tempColl);

        fp.off();
    },
);

reshardingTest.teardown();
