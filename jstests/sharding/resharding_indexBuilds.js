/**
 * Tests that the donor's state will transition to kError and reshardCollection will fail if there
 * is an ongoing index build.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

const reshardingTest = new ReshardingTest({numDonors: 1});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

// Insert a document so that the index build will hit the failpoint.
assert.commandWorked(inputCollection.insert([
    {_id: 0, oldKey: -10, newKey: -10},
]));

const mongos = inputCollection.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;

const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor_host = topology.shards[donorShardNames[0]].primary;
const donor0 = new Mongo(donor_host);

// Create an inProgress index build.
const createIndexThread = new Thread(function(host) {
    const con = new Mongo(host).getCollection("reshardingDb.coll");
    return con.createIndexes([{newKey: 1}]);
}, donor_host);
let createIndexFailpoint = configureFailPoint(donor0, "hangIndexBuildBeforeCommit");
createIndexThread.start();
createIndexFailpoint.wait();

function assertEventuallyErrorsLocally(shardConn, shardName) {
    const localDonorOpsCollection =
        shardConn.getCollection("config.localReshardingOperations.donor");

    assert.soon(
        () => {
            return localDonorOpsCollection.findOne({state: "error"}) !== null;
        },
        () => {
            return "donor shard " + shardName + " never transitioned to the error state: " +
                tojson(localDonorOpsCollection.findOne());
        });
}

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
        assertEventuallyErrorsLocally(donor0, donorShardNames[0]);
        reshardingTest.interruptReshardingThread();
    },
    ErrorCodes.Interrupted);

// Resume index build.
createIndexFailpoint.off();
createIndexThread.join();
assert.commandWorked(createIndexThread.returnData());

// Since the coordinator has already created the temporary collection locally, the ShardingTest
// teardown expects the recipients to also have the temporary collection locally for consistency.
// However, the recipients will be interrupted before the temporary collection is created locally
// due to the killing of reshardCollection command. Skipping the check allows the test to pass.
//
// TODO SERVER-52838: Remove once the coordinator cleans up its local temporary collection when the
// participants transition to error.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
reshardingTest.teardown();
})();
