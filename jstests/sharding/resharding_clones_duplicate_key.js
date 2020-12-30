/**
 * Tests that unrecoverable errors during resharding's collection cloning are handled by the
 * recipient.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 1});
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

// The following documents violate the global _id uniqueness assumption of sharded collections. It
// is possible to construct such a sharded collection due to how each shard independently enforces
// the uniqueness of _id values for only the documents it owns. The resharding operation is expected
// to abort upon discovering this violation.
assert.commandWorked(inputCollection.insert([
    {_id: 0, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: -10},
    {_id: 0, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10},
]));

function assertEventuallyErrorsLocally(shardConn, shardName) {
    const localRecipientOpsCollection =
        shardConn.getCollection("config.localReshardingOperations.recipient");

    assert.soon(
        () => {
            return localRecipientOpsCollection.findOne({state: "error"}) !== null;
        },
        () => {
            return "recipient shard " + shardName + " never transitioned to the error state: " +
                tojson(localRecipientOpsCollection.find().toArray());
        });
}

const mongos = inputCollection.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;

const topology = DiscoverTopology.findConnectedNodes(mongos);
const recipient0 = new Mongo(topology.shards[recipientShardNames[0]].primary);

const fp = configureFailPoint(recipient0, "removeRecipientDocFailpoint");

// In the current implementation, the reshardCollection command won't ever complete if one of the
// donor or recipient shards encounters an unrecoverable error. To work around this limitation, we
// verify the recipient shard transitioned itself into the "error" state as a result of the
// duplicate key error during resharding's collection cloning.
//
// TODO SERVER-50584: Remove the call to interruptReshardingThread() from this test and instead
// directly assert that the reshardCollection command fails with an error.
reshardingTest.withReshardingInBackground(  //
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        assertEventuallyErrorsLocally(recipient0, recipientShardNames[0]);
        reshardingTest.interruptReshardingThread();
    },
    ErrorCodes.Interrupted);

fp.off();

reshardingTest.teardown();
})();
