/**
 * Test for the ReshardingTest fixture itself.
 *
 * Verifies that a failing `reshardCollection` command doesn't hang a `ReshardingTest` when
 * `withReshardingInBackground` expects a success.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/discover_topology.js');
load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 2});
reshardingTest.setup();

const ns = "reshardingDb.coll";
const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const mongos = sourceCollection.getMongo();

const recipientShardNames = reshardingTest.recipientShardNames;

assert.commandWorked(mongos.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        // Choosing a random code that `reshardCollection` won't return.
        errorCode: ErrorCodes.ResumableRangeDeleterDisabled,
        failCommands: ["reshardCollection"],
    }
}));

const err = assert.throws(() => {
    reshardingTest.withReshardingInBackground({
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    });
});

assert(/ResumableRangeDeleterDisabled/.test(err.message), err);

reshardingTest.teardown();
})();
