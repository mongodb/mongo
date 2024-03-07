/**
 * Tests that provenances are matched when a _configsvrReshardCollection command is issued while
 * there is an ongoing unshard collection operation for the same collection with the same resharding
 * key,
 *
 *
 * @tags: [
 *  uses_atclustertime,
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagUnshardCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  multiversion_incompatible,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromConfigCollections, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

// Generates a new thread to run _configsvrReshardCollection.
const makeConfigsvrReshardCollectionThread = (configsvrConnString, ns) => {
    return new Thread((configsvrConnString, ns) => {
        const configsvr = new Mongo(configsvrConnString);
        assert.commandFailedWithCode(configsvr.adminCommand({
            _configsvrReshardCollection: ns,
            key: {_id: 1},
            writeConcern: {w: "majority"},
            provenance: "reshardCollection"
        }),
                                     ErrorCodes.ReshardCollectionInProgress);
    }, configsvrConnString, ns);
};

const getTempUUID = (tempNs) => {
    const tempCollection = mongos.getCollection(tempNs);
    return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
};

const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 1});
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

// Fulfilled once the unshardCollection command creates the temporary collection.
let expectedUUIDAfterReshardingCompletes = undefined;

reshardingTest.withUnshardCollectionInBackground({toShard: recipientShardNames[0]}, (tempNs) => {
    pauseBeforeCloningFP.wait();

    // The UUID of the temporary resharding collection should become the UUID of the original
    // collection once resharding has completed.

    expectedUUIDAfterReshardingCompletes = getTempUUID(tempNs);
    configsvrReshardCollectionThread.start();
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
