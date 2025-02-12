/**
 * Tests that when resharding on a shard key not covered by a secondary index, the new shard-key
 * index can be successfully created if the recipient shard is restarted during building-index
 * stage.
 * Multiversion testing does not support tests that kill and restart nodes. So we had to add the
 * 'multiversion_incompatible' tag.
 * This test restarts a replica set node, which requires persistence.
 * @tags: [
 *  requires_fcv_72,
 *  requires_persistence,
 * # TODO (SERVER-89180) Remove multiversion_incompatible tag
 *  multiversion_incompatible,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, enableElections: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
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

const reshardingPauseRecipientBeforeBuildingIndexFailpoint =
    configureFailPoint(recipient, "reshardingPauseRecipientBeforeBuildingIndex");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // Wait until participants are aware of the resharding operation.
        reshardingTest.awaitCloneTimestampChosen();
        reshardingPauseRecipientBeforeBuildingIndexFailpoint.wait();

        reshardingTest.killAndRestartPrimaryOnShard(recipientShardNames[0]);
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
