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

// The collection is cloned in ascending _id order so we insert some large documents with higher _id
// values to guarantee there will be a cursor needing to be cleaned up on the donor shards after
// cloning errors.
const largeStr = "x".repeat(9 * 1024 * 1024);
assert.commandWorked(inputCollection.insert([
    {_id: 10, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: -10, pad: largeStr},
    {_id: 11, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: -10, pad: largeStr},
    {_id: 20, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10, pad: largeStr},
    {_id: 21, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10, pad: largeStr},
]));

/**
 * Confirms the shard's abortReason and state are written locally in
 * config.localReshardingOperations.recipient, the shard's local ReshardingRecipientDocument.
 */
function assertEventuallyErrorsLocally(shardConn, shardName) {
    const localRecipientOpsCollection =
        shardConn.getCollection("config.localReshardingOperations.recipient");

    assert.soon(
        () => {
            return localRecipientOpsCollection.findOne(
                       {state: "error", "abortReason.code": ErrorCodes.DuplicateKey}) !== null;
        },
        () => {
            return "recipient shard " + shardName + " never transitioned to the error state: " +
                tojson(localRecipientOpsCollection.find().toArray());
        });
}

/**
 * Confirms the shard's abortReason and state are written in
 * config.reshardingOperations.recipientShards[shardName], the main CoordinatorDocument on the
 * configsvr.
 */
function assertEventuallyErrorsInRecipientList(configsvrConn, shardName, nss) {
    const reshardingOperationsCollection =
        configsvrConn.getCollection("config.reshardingOperations");
    assert.soon(
        () => {
            return reshardingOperationsCollection.findOne({
                nss,
                recipientShards:
                    {$elemMatch: {id: shardName, "abortReason.code": ErrorCodes.DuplicateKey}}
            }) !== null;
        },
        () => {
            return "recipient shard " + shardName + " never updated its entry in the coordinator" +
                " document to state kError and abortReason DuplicateKey: " +
                tojson(reshardingOperationsCollection.find().toArray());
        });
}

const mongos = inputCollection.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;

const topology = DiscoverTopology.findConnectedNodes(mongos);
const recipient0 = new Mongo(topology.shards[recipientShardNames[0]].primary);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

const fp = configureFailPoint(recipient0, "removeRecipientDocFailpoint");

// In the current implementation, the reshardCollection command won't ever complete if one of the
// recipients encounter an unrecoverable error while cloning. To work around this limitation, we
// verify the recipient shard transitioned itself into the "error" state as a result of the
// duplicate key error during resharding's collection cloning.
//
// TODO SERVER-53792: Investigate removing interruptReshardingThread() from this test and instead
// directly asserting that the reshardCollection command fails with an error without losing
// intermediate checks regarding where the recipient communicates its error.
reshardingTest.withReshardingInBackground(  //
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // TODO SERVER-51696: Review if these checks can be made in a cpp unittest instead.
        assertEventuallyErrorsLocally(recipient0, recipientShardNames[0]);
        assertEventuallyErrorsInRecipientList(
            configsvr, recipientShardNames[0], inputCollection.getFullName());
        reshardingTest.interruptReshardingThread();
    },
    ErrorCodes.Interrupted);

fp.off();

const idleCursors = mongos.getDB("admin")
                        .aggregate([
                            {$currentOp: {allUsers: true, idleCursors: true}},
                            {$match: {type: "idleCursor", ns: inputCollection.getFullName()}},
                        ])
                        .toArray();
assert.eq([], idleCursors, "expected cloning cursors to be cleaned up, but they weren't");

reshardingTest.teardown();
})();
