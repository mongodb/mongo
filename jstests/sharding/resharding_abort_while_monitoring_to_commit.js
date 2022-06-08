/**
 * Test that the resharding operation is aborted if any of the recipient shards encounters
 * an error during the Applying phase
 */

(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 1});
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

const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);

const recipientShardNames = reshardingTest.recipientShardNames;
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

// We have the recipient shard fail the _shardsvrReshardingOperationTime command to verify the
// ReshardingCoordinator can successfully abort the resharding operation even when the commit
// monitor doesn't see the recipient shard as being caught up enough to engage the critical section
// on the donor shards.
const shardsvrReshardingOperationTimeFailpoint = configureFailPoint(recipient, "failCommand", {
    failInternalCommands: true,
    errorCode: ErrorCodes.Interrupted,
    failCommands: ["_shardsvrReshardingOperationTime"],
});

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // We wait until cloneTimestamp has been chosen to guarantee that any subsequent writes will
        // be applied by the ReshardingOplogApplier.
        reshardingTest.awaitCloneTimestampChosen();

        // We connect directly to one of the donor shards to perform an operations which will later
        // cause the recipient shard to error during its resharding oplog application. Connecting
        // directly to the shard bypasses any synchronization which might otherwise occur from the
        // Sharding DDL Coordinator.
        const donor0Collection = donor0.getCollection(sourceCollection.getFullName());
        assert.commandWorked(donor0Collection.runCommand("collMod"));
    },
    {
        expectedErrorCode: ErrorCodes.OplogOperationUnsupported,
    });

shardsvrReshardingOperationTimeFailpoint.off();

reshardingTest.teardown();
})();
