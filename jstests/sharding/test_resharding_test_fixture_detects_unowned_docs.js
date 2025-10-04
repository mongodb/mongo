/**
 * Test for the ReshardingTest fixture itself.
 *
 * Verifies that an exception is thrown if a recipient shard has a document it doesn't actually own.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

// The test purposely emplaces documents on a shard that doesn't own them.
TestData.skipCheckOrphans = true;

const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 2});
reshardingTest.setup();

const ns = "reshardingDb.coll";
const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

// Perform some inserts before resharding starts so there's data to clone.
assert.commandWorked(
    sourceCollection.insert([
        {_id: "moves to recipient0", oldKey: -10, newKey: -10},
        {_id: "moves to recipient1", oldKey: 10, newKey: 10},
    ]),
);

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

const recipientShardNames = reshardingTest.recipientShardNames;
const recipient0 = new Mongo(topology.shards[recipientShardNames[0]].primary);

const reshardingOptions = {
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
        {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
};
const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);
if (!isMultiversion) {
    // This test explicitly inserts a document into the resharding temporary collection so
    // resharding verification is expected to fail.
    reshardingOptions.performVerification = false;
}

const err = assert.throws(() => {
    reshardingTest.withReshardingInBackground(reshardingOptions, (tempNs) => {
        // Wait for the recipients to have finished cloning so the temporary resharding
        // collection is known to exist.
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne();
            return coordinatorDoc !== null && coordinatorDoc.state === "applying";
        });

        // Insert a document directly into recipient0 that is truly owned by recipient1.
        const tempColl = recipient0.getCollection(tempNs);
        assert.commandWorked(
            tempColl.runCommand("insert", {
                documents: [{_id: "unowned by recipient0", oldKey: 10, newKey: 10}],
                shardVersion: ShardVersioningUtil.kIgnoredShardVersion,
            }),
        );
    });
});

assert(/temporary resharding collection had unowned documents/.test(err.message), err);

// The ReshardingTest fixture will have interrupted the reshardCollection command on mongos so the
// JavaScript thread running the command can be joined. The resharding operation is still active on
// config server so we must manually wait for it to complete.
assert.soon(() => {
    const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne();
    return coordinatorDoc === null;
});

reshardingTest.teardown();
