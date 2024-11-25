/**
 * Tests that if a reshardCollection command with a user-provided reshardingUUID is completed,
 * then after failover the result is available to retries.
 *
 * @tags: [
 *   featureFlagReshardingImprovements,
 *   requires_fcv_72,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 *   uses_atclustertime,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromConfigCollections, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

// TODO SERVER-88620 Re-enable the check.
TestData.skipCheckRoutingTableConsistency = true;

const enterAbortFailpointName = "reshardingPauseCoordinatorBeforeStartingErrorFlow";
const originalReshardingUUID = UUID();
const newReshardingUUID = UUID();

const getTempUUID = (tempNs) => {
    const tempCollection = mongos.getCollection(tempNs);
    return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
};

const reshardingTest = new ReshardingTest(
    {numDonors: 1, minimumOperationDurationMS: 0, initiateWithDefaultElectionTimeout: true});
reshardingTest.setup();
const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const mongos = sourceCollection.getMongo();
let topology = DiscoverTopology.findConnectedNodes(mongos);
let configsvr = new Mongo(topology.configsvr.primary);

if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
    jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
    reshardingTest.teardown();
    quit();
}

let pauseBeforeCloningFP = configureFailPoint(configsvr, "reshardingPauseCoordinatorBeforeCloning");

// Fulfilled once the first reshardCollection command creates the temporary collection.
let expectedUUIDAfterReshardingCompletes = undefined;

const generateAbortThread = (mongosConnString, ns) => {
    return new Thread((mongosConnString, ns) => {
        const mongos = new Mongo(mongosConnString);
        assert.commandWorked(mongos.adminCommand({abortReshardCollection: ns}));
    }, mongosConnString, ns);
};

let abortThread = generateAbortThread(mongos.host, sourceCollection.getFullName());

jsTestLog("Attempting a resharding that will abort, with UUID: " + originalReshardingUUID);
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        reshardingUUID: originalReshardingUUID,
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        pauseBeforeCloningFP.wait();

        const enterAbortFailpoint = configureFailPoint(configsvr, enterAbortFailpointName);
        abortThread.start();
        enterAbortFailpoint.wait();
        enterAbortFailpoint.off();

        pauseBeforeCloningFP.off();
    },
    {
        expectedErrorCode: ErrorCodes.ReshardCollectionAborted,
    });
abortThread.join();

// Confirm the collection UUID did not change.
let finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(reshardingTest.sourceCollectionUUID, finalSourceCollectionUUID);

jsTestLog("Retrying aborted resharding with UUID: " + originalReshardingUUID);
// A retry after the fact with the same UUID should not attempt to reshard the collection again,
// and also should return same error code.
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: originalReshardingUUID
}),
                             ErrorCodes.ReshardCollectionAborted);
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(reshardingTest.sourceCollectionUUID, finalSourceCollectionUUID);

// Makes sure the same thing happens after failover
reshardingTest.shutdownAndRestartPrimaryOnShard(reshardingTest.configShardName);
topology = DiscoverTopology.findConnectedNodes(mongos);
configsvr = new Mongo(topology.configsvr.primary);

jsTestLog("After failover, retrying aborted resharding with UUID: " + originalReshardingUUID);
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: originalReshardingUUID
}),
                             ErrorCodes.ReshardCollectionAborted);
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(reshardingTest.sourceCollectionUUID, finalSourceCollectionUUID);

// Try it again but let it succeed this time.
jsTestLog("Trying resharding with new UUID: " + newReshardingUUID);
reshardingTest.retryOnceOnNetworkError(() => {
    pauseBeforeCloningFP = configureFailPoint(configsvr, "reshardingPauseCoordinatorBeforeCloning");
});
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    reshardingUUID: newReshardingUUID,
    newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
},
                                          (tempNs) => {
                                              pauseBeforeCloningFP.wait();

                                              // The UUID of the temporary resharding collection
                                              // should become the UUID of the original collection
                                              // once resharding has completed.
                                              expectedUUIDAfterReshardingCompletes =
                                                  getTempUUID(tempNs);

                                              pauseBeforeCloningFP.off();
                                          });

// Resharding should have succeeded.
assert.neq(expectedUUIDAfterReshardingCompletes, undefined);
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

jsTestLog("After completion, retrying resharding with UUID: " + newReshardingUUID);
// A retry after the fact with the same UUID should not attempt to reshard the collection again,
// and should succeed.
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: newReshardingUUID
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

// Makes sure the same thing happens after failover
reshardingTest.shutdownAndRestartPrimaryOnShard(reshardingTest.configShardName);
topology = DiscoverTopology.findConnectedNodes(mongos);
configsvr = new Mongo(topology.configsvr.primary);

jsTestLog("After completion and failover, retrying resharding with UUID: " + newReshardingUUID);
assert.commandWorked(mongos.adminCommand({
    reshardCollection: sourceCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: reshardingTest.presetReshardedChunks,
    reshardingUUID: newReshardingUUID
}));
finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

reshardingTest.teardown();
