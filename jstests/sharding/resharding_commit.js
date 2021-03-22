/**
 * Test the commitReshardCollection command.
 *
 * @tags: [requires_fcv_49, uses_atclustertime]
 */
(function() {
"use strict";
load("jstests/sharding/libs/resharding_test_fixture.js");

const sourceNs = "reshardingDb.coll";

const reshardingTest = new ReshardingTest({
    numDonors: 2,
    numRecipients: 2,
    reshardInPlace: true,
    commitImplicitly: false,
});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const inputCollection = reshardingTest.createShardedCollection({
    ns: sourceNs,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        const mongos = inputCollection.getMongo();
        reshardingTest.awaitCloneTimestampChosen();
        assert.commandWorked(mongos.adminCommand({commitReshardCollection: sourceNs}));
    });

reshardingTest.teardown();
})();
