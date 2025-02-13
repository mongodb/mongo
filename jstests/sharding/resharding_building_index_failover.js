/**
 * Tests that when resharding is in building-index phase, failover happens and resharding should
 * still work correctly.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const ns = "reshardingDb.coll";
const reshardingTest = new ReshardingTest({numDonors: 2, enableElections: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});
const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

const recipientShardNames = reshardingTest.recipientShardNames;
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
    jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
    reshardingTest.teardown();
    quit();
}

// Create an index on oldKey.
assert.commandWorked(
    mongos.getCollection(ns).insert([{oldKey: 1, newKey: -1}, {oldKey: 2, newKey: -2}]));
assert.commandWorked(mongos.getCollection(ns).createIndex({oldKey: 1}));
const hangAfterInitializingIndexBuildFailPoint =
    configureFailPoint(recipient, "hangAfterInitializingIndexBuild");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // Wait until participants are aware of the resharding operation.
        reshardingTest.awaitCloneTimestampChosen();
        hangAfterInitializingIndexBuildFailPoint.wait();
        jsTestLog("Hang primary during building index, then step up a new primary");

        reshardingTest.stepUpNewPrimaryOnShard(recipientShardNames[0]);
        const recipientRS = reshardingTest.getReplSetForShard(recipientShardNames[0]);
        recipientRS.awaitSecondaryNodes();
        recipientRS.awaitReplication();
        reshardingTest.retryOnceOnNetworkError(hangAfterInitializingIndexBuildFailPoint.off);
    },
    {
        afterReshardingFn: () => {
            const indexes = mongos.getDB("reshardingDb").getCollection("coll").getIndexes();
            let haveNewShardKeyIndex = false;
            indexes.forEach(index => {
                if ("newKey" in index["key"]) {
                    haveNewShardKeyIndex = true;
                }
            });
            assert.eq(haveNewShardKeyIndex, true);
        }
    });

reshardingTest.teardown();
