/**
 * Test for the ReshardingTest fixture itself.
 *
 * Verifies that the background thread running the reshardCollection command will retry when mongos
 * reports an error caused by a network error from the primary shard.
 *
 * @tags: [
 *   requires_persistence,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({enableElections: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const primaryShard = donorShardNames[0];

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    primaryShardName: primaryShard,
});

assert.commandWorked(sourceCollection.insert({oldKey: 1, newKey: 2}));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // We use awaitCloneTimestampChosen() for syntactic convenience to wait for the
        // _shardsvrReshardCollection command to have been received by the primary shard.
        reshardingTest.awaitCloneTimestampChosen();

        // Mongos uses the ARS for running the _shardsvrReshardCollection command and will retry up
        // to 3 times on a network error. We restart the server more than that many times to
        // exercise the logic of the ReshardingTest fixture needing to retry the whole
        // reshardCollection command.
        const numRestarts = 5;
        for (let i = 0; i < numRestarts; ++i) {
            reshardingTest.shutdownAndRestartPrimaryOnShard(primaryShard);
        }
    });

reshardingTest.teardown();
})();
