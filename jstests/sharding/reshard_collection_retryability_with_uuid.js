/**
 * Tests that if a reshardCollection command is issued while there is an ongoing
 * resharding operation for the same resharding UUID, the command joins with the ongoing
 * resharding instance. But a reshardCollection command for the same collection with the
 * same resharding key but a different UUID or no UUID should fail. Further, after the
 * resharding operation completes, reshardCollection with the same UUID should receive the results
 * even if forceRedistribution is true.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_72,
 *   featureFlagReshardingImprovements,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromConfigCollections, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const originalReshardingUUID = UUID();

// Generates a new thread to run subsequent reshardCollections.  This command must be exactly the
// same as the original resharding command we're trying to retry.
const makeReshardCollectionThread =
    (routerConnString, ns, presetReshardedChunks, reshardingUUID, forceRedistribution) => {
        if (reshardingUUID)
            reshardingUUID = reshardingUUID.toString();
        return new Thread(
            (routerConnString, ns, presetReshardedChunks, reshardingUUID, forceRedistribution) => {
                const s = new Mongo(routerConnString);
                let command = {
                    reshardCollection: ns,
                    key: {newKey: 1},
                    _presetReshardedChunks: presetReshardedChunks
                };
                if (reshardingUUID !== undefined) {
                    reshardingUUID = eval(reshardingUUID);
                    command = Object.merge(command, {reshardingUUID: reshardingUUID});
                }
                if (forceRedistribution !== undefined) {
                    command = Object.merge(command, {forceRedistribution: forceRedistribution});
                }
                assert.commandWorked(s.adminCommand(command));
            },
            routerConnString,
            ns,
            presetReshardedChunks,
            reshardingUUID,
            forceRedistribution);
    };

const getTempUUID = (tempNs) => {
    const tempCollection = mongos.getCollection(tempNs);
    return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
};

const reshardingTest = new ReshardingTest({numDonors: 1, minimumOperationDurationMS: 0});
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

if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
    jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
    reshardingTest.teardown();
    quit();
}

const pauseBeforeCloningFP =
    configureFailPoint(configsvr, "reshardingPauseCoordinatorBeforeCloning");

// Fulfilled once the first reshardCollection command creates the temporary collection.
let expectedUUIDAfterReshardingCompletes = undefined;

let reshardCollectionThread;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        reshardingUUID: originalReshardingUUID,
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    (tempNs) => {
        pauseBeforeCloningFP.wait();

        // The UUID of the temporary resharding collection should become the UUID of the original
        // collection once resharding has completed.
        expectedUUIDAfterReshardingCompletes = getTempUUID(tempNs);

        reshardCollectionThread = makeReshardCollectionThread(mongos.host,
                                                              sourceCollection.getFullName(),
                                                              reshardingTest.presetReshardedChunks,
                                                              originalReshardingUUID);

        // Trying to reconnect using a different resharding UUID should not work.  This
        // tests the config server command directly because otherwise the
        // ReshardCollectionCoordinator on the primary shard would reject the command.
        assert.commandFailedWithCode(configsvr.adminCommand({
            _configsvrReshardCollection: sourceCollection.getFullName(),
            reshardingUUID: UUID(),
            key: {newKey: 1},
            writeConcern: {w: "majority"},
            provenance: "reshardCollection"
        }),
                                     ErrorCodes.ReshardCollectionInProgress);

        // Trying to reconnect using no resharding UUID should not work either.
        assert.commandFailedWithCode(configsvr.adminCommand({
            _configsvrReshardCollection: sourceCollection.getFullName(),
            key: {newKey: 1},
            writeConcern: {w: "majority"},
            provenance: "reshardCollection"
        }),
                                     ErrorCodes.ReshardCollectionInProgress);

        reshardCollectionThread.start();

        pauseBeforeCloningFP.off();
    });

reshardCollectionThread.join();

// Confirm the UUID for the namespace that was resharded is the same as the temporary collection's
// UUID before the second reshardCollection command was issued.
assert.neq(expectedUUIDAfterReshardingCompletes, undefined);
let finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

// A retry after the fact with the same UUID should not reshard the collection again.
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: originalReshardingUUID
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

// A retry after the fact with the same UUID and forceRedistribution should not reshard the
// collection again.
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: originalReshardingUUID,
    forceRedistribution: true
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

// A retry after the fact with no UUID should not reshard the collection again (because the key
// is already the same).
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

const newReshardingUUID = UUID();
// A retry after the fact with a new UUID should not reshard the collection again (because
// forceRedistribution was not specified and the key has not changed)
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: newReshardingUUID
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

// A retry after the fact with a new UUID and forceRedistribution SHOULD reshard the collection
// again.
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: newReshardingUUID,
    forceRedistribution: true
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.neq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

// A retry after the fact with no UUID and forceRedistribution SHOULD reshard the collection again.
let newSourceCollectionUUID = finalSourceCollectionUUID;
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    forceRedistribution: true
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.neq(newSourceCollectionUUID, finalSourceCollectionUUID);

reshardingTest.teardown();
