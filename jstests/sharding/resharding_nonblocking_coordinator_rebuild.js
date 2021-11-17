/**
 * Tests that resharding participants do not block replication while waiting for the
 * ReshardingCoordinatorService to be rebuilt.
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

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

const reshardingPauseRecipientBeforeCloningFailpoint =
    configureFailPoint(recipient, "reshardingPauseRecipientBeforeCloning");

// We prevent primary-only service Instances from being constructed on all of the config server
// replica set because we don't know which node will be elected primary from calling
// stepUpNewPrimaryOnShard().
const possibleCoordinators = topology.configsvr.nodes.map(host => new Mongo(host));
const pauseBeforeConstructingCoordinatorsFailpointList = possibleCoordinators.map(
    conn => configureFailPoint(conn, "PrimaryOnlyServiceHangBeforeRebuildingInstances"));

// The ReshardingTest fixture had enabled failpoints on the original config server primary so it
// could safely perform data consistency checks. It doesn't handle those failpoints not taking
// effect on the new config server primary. We intentionally have the resharding operation abort to
// skip those data consistency checks and work around this limitation.
//
// forceRecipientToLaterFailReshardingOp() is written as a helper function this way so it doesn't
// distract from the body of the withReshardingInBackground() callback function because that part is
// the true part of the test.
const forceRecipientToLaterFailReshardingOp = (fn) => {
    // Note that it is safe to enable the reshardingPauseRecipientDuringOplogApplication failpoint
    // after the resharding operation has begun because this test already enabled the
    // reshardingPauseRecipientBeforeCloning failpoint.
    const reshardingPauseRecipientDuringOplogApplicationFailpoint =
        configureFailPoint(recipient, "reshardingPauseRecipientDuringOplogApplication");

    fn();

    // The following documents violate the global _id uniqueness assumption of sharded collections.
    // It is possible to construct such a sharded collection due to how each shard independently
    // enforces the uniqueness of _id values for only the documents it owns. The resharding
    // operation is expected to abort upon discovering this violation.
    assert.commandWorked(sourceCollection.insert([
        {_id: 0, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: 10},
        {_id: 0, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10},
    ]));

    reshardingPauseRecipientDuringOplogApplicationFailpoint.off();
};

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // Wait until participants are aware of the resharding operation.
        reshardingTest.awaitCloneTimestampChosen();
        reshardingPauseRecipientBeforeCloningFailpoint.wait();

        forceRecipientToLaterFailReshardingOp(() => {
            reshardingTest.stepUpNewPrimaryOnShard(reshardingTest.configShardName);
            reshardingPauseRecipientBeforeCloningFailpoint.off();
        });

        // Verify the update from the recipient shard is able to succeed despite the
        // ReshardingCoordinatorService not having been rebuilt yet.
        let coordinatorDoc;
        assert.soon(() => {
            coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: sourceCollection.getFullName()
            });

            const recipientShardEntry =
                coordinatorDoc.recipientShards.find(shard => shard.id === recipientShardNames[0]);
            const recipientState = recipientShardEntry.mutableState.state;
            return recipientState === "applying";
        }, () => `recipient never transitioned to the "applying" state: ${tojson(coordinatorDoc)}`);

        // Also verify the config server replica set can replicate writes to a majority of its
        // members because that is originally how this issue around holding open an oplog hole had
        // manifested.
        assert.commandWorked(mongos.getCollection("config.dummycoll").insert({}, {w: "majority"}));

        // The update from the recipient shard is still waiting for the ReshardingCoordinatorService
        // to be rebuilt but should have any interruptions be non-fatal for the mongod process.
        reshardingTest.stepUpNewPrimaryOnShard(reshardingTest.configShardName);

        for (let fp of pauseBeforeConstructingCoordinatorsFailpointList) {
            reshardingTest.retryOnceOnNetworkError(() => fp.off());
        }
    },
    {
        expectedErrorCode: 5356800,
    });

reshardingTest.teardown();
})();
