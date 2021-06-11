/**
 * Tests that if a _configsvrReshardCollection command is issued while there is an ongoing
 * resharding operation for the same collection with the same resharding key, the command joins with
 * the ongoing resharding instance.
 *
 * Use _configsvrReshardCollection instead of reshardCollection to exercise the behavior of the
 * config server in the absence of the distributed lock taken by _shardsvrReshardCollection on the
 * primary shard for the database.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/libs/parallelTester.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

// Generates a new thread to run _configsvrReshardCollection.
const makeConfigsvrReshardCollectionThread = (configsvrConnString, ns) => {
    return new Thread((configsvrConnString, ns) => {
        const configsvr = new Mongo(configsvrConnString);
        assert.commandWorked(configsvr.adminCommand(
            {_configsvrReshardCollection: ns, key: {newKey: 1}, writeConcern: {w: "majority"}}));
    }, configsvrConnString, ns);
};

const getTempUUID = (tempNs) => {
    const tempCollection = mongos.getCollection(tempNs);
    return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
};

const reshardingTest = new ReshardingTest({numDonors: 1});
reshardingTest.setup();
const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

const pauseBeforeCloningFP =
    configureFailPoint(configsvr, "reshardingPauseCoordinatorBeforeCloning");

const configsvrReshardCollectionThread = makeConfigsvrReshardCollectionThread(
    topology.configsvr.nodes[0], sourceCollection.getFullName());

// Fulfilled once the first reshardCollection command creates the temporary collection.
let expectedUUIDAfterReshardingCompletes = undefined;

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    (tempNs) => {
        pauseBeforeCloningFP.wait();

        // The UUID of the temporary resharding collection should become the UUID of the original
        // collection once resharding has completed.
        expectedUUIDAfterReshardingCompletes = getTempUUID(tempNs);

        const reshardCollectionJoinedFP =
            configureFailPoint(configsvr, "reshardCollectionJoinedExistingOperation");

        configsvrReshardCollectionThread.start();

        // Hitting the reshardCollectionJoinedFP is additional confirmation that
        // _configsvrReshardCollection command (identical resharding key and collection as the
        // ongoing operation) gets joined with the ongoing resharding operation.
        reshardCollectionJoinedFP.wait();

        reshardCollectionJoinedFP.off();
        pauseBeforeCloningFP.off();
    });

configsvrReshardCollectionThread.join();

// Confirm the UUID for the namespace that was resharded is the same as the temporary collection's
// UUID before the second reshardCollection command was issued.
assert.neq(expectedUUIDAfterReshardingCompletes, undefined);
const finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

reshardingTest.teardown();
})();
