/**
 * Shared helper functions for resharding failover tests and their timeseries variants.
 *
 * Each exported `run*` function contains the full test body for one test pair. The non-timeseries
 * and timeseries callers pass a config object that captures only the differences between the two
 * variants (shard key patterns, chunk boundaries, collection options, and namespace callbacks).
 *
 * Because shard names are assigned at runtime by ReshardingTest, `chunks` and `newChunks` in each
 * config must be factory functions:
 *   chunks: (donorShardNames, recipientShardNames) => [...array of chunk objects...]
 *   newChunks: (donorShardNames, recipientShardNames) => [...array of chunk objects...]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromConfigCollections, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

/**
 * Runs the retry-after-failover resharding test.
 *
 * @param {Object} config
 *   @param {Object}   config.shardKeyPattern    - Initial shard key pattern.
 *   @param {Function} config.chunks             - (donorShardNames, recipientShardNames) => Array.
 *                                                 Initial chunk distribution.
 *   @param {Object}   [config.shardCollOptions] - Extra options for createShardedCollection
 *                                                 (e.g. {timeseries: ...}).
 *   @param {Object}   config.newShardKeyPattern - Target shard key pattern.
 *   @param {Function} config.newChunks          - (donorShardNames, recipientShardNames) => Array.
 *                                                 Target chunk distribution.
 *   @param {Function} config.getAbortNs         - (coll) => ns string for abortReshardCollection.
 *   @param {Function} config.getCollUUID        - (db, coll) => UUID for UUID equality checks.
 *   @param {Function} [config.postReshardingFn] - Optional (mongos) => void, run before teardown.
 */
export function runRetryAfterFailover(config) {
    const enterAbortFailpointName = "reshardingPauseCoordinatorBeforeStartingErrorFlow";
    const originalReshardingUUID = UUID();
    const newReshardingUUID = UUID();

    const reshardingTest = new ReshardingTest({
        numDonors: 1,
        minimumOperationDurationMS: 0,
        initiateWithDefaultElectionTimeout: true,
    });
    reshardingTest.setup();
    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;

    const createCollOpts = {
        ns: "reshardingDb.coll",
        shardKeyPattern: config.shardKeyPattern,
        chunks: config.chunks(donorShardNames, recipientShardNames),
    };
    if (config.shardCollOptions) {
        createCollOpts.shardCollOptions = config.shardCollOptions;
    }
    const sourceCollection = reshardingTest.createShardedCollection(createCollOpts);

    const mongos = sourceCollection.getMongo();
    let topology = DiscoverTopology.findConnectedNodes(mongos);
    let configsvr = new Mongo(topology.configsvr.primary);

    const getTempUUID = (tempNs) => {
        const tempCollection = mongos.getCollection(tempNs);
        return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
    };

    let pauseBeforeCloningFP = configureFailPoint(configsvr, "reshardingPauseCoordinatorBeforeCloning");

    // Fulfilled once the first reshardCollection command creates the temporary collection.
    let expectedUUIDAfterReshardingCompletes = undefined;

    const generateAbortThread = (mongosConnString, ns) => {
        return new Thread(
            (mongosConnString, ns) => {
                const mongos = new Mongo(mongosConnString);
                assert.commandWorked(mongos.adminCommand({abortReshardCollection: ns}));
            },
            mongosConnString,
            ns,
        );
    };

    const resolvedNewChunks = config.newChunks(donorShardNames, recipientShardNames);

    let abortThread = generateAbortThread(mongos.host, config.getAbortNs(sourceCollection));

    jsTestLog("Attempting a resharding that will abort, with UUID: " + originalReshardingUUID);
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: config.newShardKeyPattern,
            reshardingUUID: originalReshardingUUID,
            newChunks: resolvedNewChunks,
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
        },
    );
    abortThread.join();

    jsTestLog("Retrying aborted resharding with UUID: " + originalReshardingUUID);
    // A retry after the fact with the same UUID should not attempt to reshard the collection again,
    // and also should return same error code.
    assert.commandFailedWithCode(
        mongos.adminCommand({
            reshardCollection: sourceCollection.getFullName(),
            key: config.newShardKeyPattern,
            _presetReshardedChunks: reshardingTest.presetReshardedChunks,
            reshardingUUID: originalReshardingUUID,
        }),
        ErrorCodes.ReshardCollectionAborted,
    );
    let finalSourceCollectionUUID = config.getCollUUID(sourceCollection.getDB(), sourceCollection);
    assert.eq(reshardingTest.sourceCollectionUUID, finalSourceCollectionUUID);

    // Makes sure the same thing happens after failover
    reshardingTest.shutdownAndRestartPrimaryOnShard(reshardingTest.configShardName);
    topology = DiscoverTopology.findConnectedNodes(mongos);
    configsvr = new Mongo(topology.configsvr.primary);

    jsTestLog("After failover, retrying aborted resharding with UUID: " + originalReshardingUUID);
    assert.commandFailedWithCode(
        mongos.adminCommand({
            reshardCollection: sourceCollection.getFullName(),
            key: config.newShardKeyPattern,
            _presetReshardedChunks: reshardingTest.presetReshardedChunks,
            reshardingUUID: originalReshardingUUID,
        }),
        ErrorCodes.ReshardCollectionAborted,
    );
    finalSourceCollectionUUID = config.getCollUUID(sourceCollection.getDB(), sourceCollection);
    assert.eq(reshardingTest.sourceCollectionUUID, finalSourceCollectionUUID);

    // Try it again but let it succeed this time.
    jsTestLog("Trying resharding with new UUID: " + newReshardingUUID);
    reshardingTest.retryOnceOnNetworkError(() => {
        pauseBeforeCloningFP = configureFailPoint(configsvr, "reshardingPauseCoordinatorBeforeCloning");
    });
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: config.newShardKeyPattern,
            reshardingUUID: newReshardingUUID,
            newChunks: resolvedNewChunks,
        },
        (tempNs) => {
            pauseBeforeCloningFP.wait();

            // The UUID of the temporary resharding collection
            // should become the UUID of the original collection
            // once resharding has completed.
            expectedUUIDAfterReshardingCompletes = getTempUUID(tempNs);

            pauseBeforeCloningFP.off();
        },
    );

    // Resharding should have succeeded.
    assert.neq(expectedUUIDAfterReshardingCompletes, undefined);
    finalSourceCollectionUUID = config.getCollUUID(sourceCollection.getDB(), sourceCollection);
    assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

    jsTestLog("After completion, retrying resharding with UUID: " + newReshardingUUID);
    // A retry after the fact with the same UUID should not attempt to reshard the collection again,
    // and should succeed.
    assert.commandWorked(
        mongos.adminCommand({
            reshardCollection: sourceCollection.getFullName(),
            key: config.newShardKeyPattern,
            _presetReshardedChunks: reshardingTest.presetReshardedChunks,
            reshardingUUID: newReshardingUUID,
        }),
    );
    finalSourceCollectionUUID = config.getCollUUID(sourceCollection.getDB(), sourceCollection);
    assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

    // Makes sure the same thing happens after failover
    reshardingTest.shutdownAndRestartPrimaryOnShard(reshardingTest.configShardName);
    topology = DiscoverTopology.findConnectedNodes(mongos);
    configsvr = new Mongo(topology.configsvr.primary);

    jsTestLog("After completion and failover, retrying resharding with UUID: " + newReshardingUUID);
    assert.commandWorked(
        mongos.adminCommand({
            reshardCollection: sourceCollection.getFullName(),
            key: config.newShardKeyPattern,
            _presetReshardedChunks: reshardingTest.presetReshardedChunks,
            reshardingUUID: newReshardingUUID,
        }),
    );
    finalSourceCollectionUUID = config.getCollUUID(sourceCollection.getDB(), sourceCollection);
    assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

    if (config.postReshardingFn) {
        config.postReshardingFn(mongos);
    }

    reshardingTest.teardown();
}

/**
 * Runs the failover-during-abort resharding test.
 *
 * @param {Object} config
 *   @param {Object}   config.shardKeyPattern    - Initial shard key pattern.
 *   @param {Function} config.chunks             - (donorShardNames, recipientShardNames) => Array.
 *                                                 Initial chunk distribution.
 *   @param {Object}   [config.shardCollOptions] - Extra options for createShardedCollection.
 *   @param {Object}   config.newShardKeyPattern - Target shard key pattern.
 *   @param {Function} config.newChunks          - (donorShardNames, recipientShardNames) => Array.
 *                                                 Target chunk distribution.
 *   @param {Function} config.getRecipientDocNs  - (coll) => ns string for recipient doc lookup.
 *   @param {Function} [config.postReshardingFn] - Optional (mongos) => void, run before teardown.
 */
export function runFailoverDuringAbort(config) {
    const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;

    const createCollOpts = {
        ns: "reshardingDb.coll",
        shardKeyPattern: config.shardKeyPattern,
        chunks: config.chunks(donorShardNames, recipientShardNames),
    };
    if (config.shardCollOptions) {
        createCollOpts.shardCollOptions = config.shardCollOptions;
    }
    const sourceCollection = reshardingTest.createShardedCollection(createCollOpts);
    const mongos = sourceCollection.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(mongos);
    const donor = new Mongo(topology.shards[donorShardNames[0]].primary);
    const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

    const reshardingDonorFailsBeforeObtainingTimestampFp = configureFailPoint(
        donor,
        "reshardingDonorFailsBeforeObtainingTimestamp",
    );
    const hangBeforeRemovingRecipientDocFp = configureFailPoint(recipient, "removeRecipientDocFailpoint");

    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: config.newShardKeyPattern,
            newChunks: config.newChunks(donorShardNames, recipientShardNames),
        },
        () => {
            hangBeforeRemovingRecipientDocFp.wait();

            const recipientDoc = recipient.getCollection("config.localReshardingOperations.recipient").findOne({
                ns: config.getRecipientDocNs(sourceCollection),
            });
            assert(recipientDoc != null);
            assert(recipientDoc.mutableState.state === "done");
            assert(recipientDoc.mutableState.abortReason != null);
            assert(recipientDoc.mutableState.abortReason.code === ErrorCodes.ReshardCollectionAborted);

            reshardingTest.stepUpNewPrimaryOnShard(recipientShardNames[0]);
            const recipientRS = reshardingTest.getReplSetForShard(recipientShardNames[0]);
            recipientRS.awaitSecondaryNodes();
            recipientRS.awaitReplication();
            reshardingTest.retryOnceOnNetworkError(hangBeforeRemovingRecipientDocFp.off);
        },
        {expectedErrorCode: ErrorCodes.InternalError},
    );

    if (config.postReshardingFn) {
        config.postReshardingFn(mongos);
    }

    reshardingTest.teardown();
}

/**
 * Runs the building-index-failover resharding test.
 *
 * @param {Object} config
 *   @param {Object}   config.shardKeyPattern       - Initial shard key pattern.
 *   @param {Function} config.chunks                - (donorShardNames, recipientShardNames) => Array.
 *                                                    Initial chunk distribution.
 *   @param {Object}   [config.shardCollOptions]    - Extra options for createShardedCollection.
 *   @param {Object}   config.newShardKeyPattern    - Target shard key pattern.
 *   @param {Function} config.newChunks             - (donorShardNames, recipientShardNames) => Array.
 *                                                    Target chunk distribution.
 *   @param {Array}    config.documents             - Documents to insert before resharding.
 *   @param {Object}   config.indexKey              - Index to create before resharding.
 *   @param {string}   config.newShardKeyIndexField - Field name to look for in post-reshard indexes.
 *   @param {Function} [config.postReshardingFn]    - Optional (mongos) => void, run before teardown.
 */
export function runBuildingIndexFailover(config) {
    const ns = "reshardingDb.coll";
    const reshardingTest = new ReshardingTest({numDonors: 2, enableElections: true});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;

    const createCollOpts = {
        ns,
        shardKeyPattern: config.shardKeyPattern,
        chunks: config.chunks(donorShardNames, recipientShardNames),
    };
    if (config.shardCollOptions) {
        createCollOpts.shardCollOptions = config.shardCollOptions;
    }
    const sourceCollection = reshardingTest.createShardedCollection(createCollOpts);
    const mongos = sourceCollection.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(mongos);

    const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

    // Create an index on the shard key field.
    assert.commandWorked(mongos.getCollection(ns).insert(config.documents));
    assert.commandWorked(mongos.getCollection(ns).createIndex(config.indexKey));
    const hangAfterInitializingIndexBuildFailPoint = configureFailPoint(recipient, "hangAfterInitializingIndexBuild");

    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: config.newShardKeyPattern,
            newChunks: config.newChunks(donorShardNames, recipientShardNames),
        },
        () => {
            // Wait until participants are aware of the resharding operation.
            reshardingTest.awaitCloneTimestampChosen();
            hangAfterInitializingIndexBuildFailPoint.wait();
            jsTestLog("Hang primary during building index, then step up a new primary");

            reshardingTest.stepUpNewPrimaryOnShard(recipientShardNames[0]);
            const recipientRS = reshardingTest.getReplSetForShard(recipientShardNames[0]);
            recipientRS.awaitSecondaryNodes();
            recipientRS.awaitReplication();
            reshardingTest.retryOnceOnNetworkError(hangAfterInitializingIndexBuildFailPoint.off);
        },
        {
            afterReshardingFn: () => {
                const indexes = mongos.getDB("reshardingDb").getCollection("coll").getIndexes();
                const haveNewShardKeyIndex = indexes.some((index) => config.newShardKeyIndexField in index["key"]);
                assert.eq(haveNewShardKeyIndex, true);
            },
        },
    );

    if (config.postReshardingFn) {
        config.postReshardingFn(mongos);
    }

    reshardingTest.teardown();
}

/**
 * Runs the failover-shutdown-basic resharding test.
 *
 * @param {Object} config
 *   @param {Object}   config.shardKeyPattern    - Initial shard key pattern.
 *   @param {Function} config.chunks             - (donorShardNames, recipientShardNames) => Array.
 *                                                 Initial chunk distribution.
 *   @param {Object}   [config.shardCollOptions] - Extra options for createShardedCollection.
 *   @param {Object}   config.newShardKeyPattern - Target shard key pattern.
 *   @param {Function} config.newChunks          - (donorShardNames, recipientShardNames) => Array.
 *                                                 Target chunk distribution.
 *   @param {Array}    config.documents          - Documents to insert before resharding.
 *   @param {Function} [config.postReshardingFn] - Optional (mongos) => void, run before teardown.
 */
export function runFailoverShutdownBasic(config) {
    const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, enableElections: true});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;

    const createCollOpts = {
        ns: "reshardingDb.coll",
        shardKeyPattern: config.shardKeyPattern,
        chunks: config.chunks(donorShardNames, recipientShardNames),
    };
    if (config.shardCollOptions) {
        createCollOpts.shardCollOptions = config.shardCollOptions;
    }
    const sourceCollection = reshardingTest.createShardedCollection(createCollOpts);

    assert.commandWorked(sourceCollection.insert(config.documents));

    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: config.newShardKeyPattern,
            newChunks: config.newChunks(donorShardNames, recipientShardNames),
        },
        () => {
            reshardingTest.stepUpNewPrimaryOnShard(donorShardNames[0]);

            reshardingTest.killAndRestartPrimaryOnShard(recipientShardNames[0]);

            reshardingTest.shutdownAndRestartPrimaryOnShard(recipientShardNames[1]);
        },
    );

    if (config.postReshardingFn) {
        config.postReshardingFn(sourceCollection.getMongo());
    }

    reshardingTest.teardown();
}
