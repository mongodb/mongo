/**
 * Tests that unrecoverable errors during resharding's oplog application are handled by the
 * recipient.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

const recipientShardNames = reshardingTest.recipientShardNames;

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]}
        ],
    },
    () => {
        const mongos = inputCollection.getMongo();
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: inputCollection.getFullName()
            });
            return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
        });

        // The following documents violate the global _id uniqueness assumption of sharded
        // collections. It is possible to construct such a sharded collection due to how each shard
        // independently enforces the uniqueness of _id values for only the documents it owns. The
        // resharding operation is expected to abort upon discovering this violation.
        assert.commandWorked(inputCollection.insert([
            {_id: 0, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: 10},
            {_id: 0, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10},
        ]));
    },
    {
        expectedErrorCode: 5356800,
    });

reshardingTest.teardown();
})();
