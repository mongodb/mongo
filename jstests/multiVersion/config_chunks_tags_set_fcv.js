/**
 * Tests that config.chunks and config.tags documents are correctly modified on FCV
 * upgrade/downgrade.
 */
(function() {
"use strict";

load("jstests/libs/parallelTester.js");  // for Thread.
load("jstests/multiVersion/libs/config_chunks_tags_shared.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

// Assumes ns has the following chunk layout: [-inf, -50), [-50, 0) on shard0 and [0, inf) on
// shard 1.
function verifyChunkOperationsFailDuringSetFCV(st, ns) {
    assert.commandFailedWithCode(st.s.adminCommand({split: ns, middle: {_id: 50}}),
                                 ErrorCodes.ConflictingOperationInProgress);
    verifyChunkDistribution(st, ns, [2, 1]);

    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}),
        ErrorCodes.ConflictingOperationInProgress);
    verifyChunkDistribution(st, ns, [2, 1]);

    assert.commandFailedWithCode(
        st.s.adminCommand({mergeChunks: ns, bounds: [{_id: MinKey}, {_id: 0}]}),
        ErrorCodes.ConflictingOperationInProgress);
    verifyChunkDistribution(st, ns, [2, 1]);
}

// Assumes shard0 is in zone0 which contains [-inf, 0) and is not in zone1.
function verifyZoneOperationsSucceedDuringSetFCV(st, ns) {
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: null}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: "zone0"}));

    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zone1"}));
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: "zone1"}));
}

const dbName = "test";
const chunkNs = dbName + ".chunk_coll";
const zoneNs = dbName + ".zone_coll";

const st = new ShardingTest({shards: 2});
const configPrimary = st.configRS.getPrimary();

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

setUpCollectionForChunksTesting(st, chunkNs);
setUpCollectionForZoneTesting(st, zoneNs);

//
// Verify chunk and tag documents are updated by setFeatureCompatibilityVersion.
//

checkFCV(configPrimary.getDB("admin"), latestFCV);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: true});

jsTestLog("Downgrading FCV to last stable");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(configPrimary.getDB("admin"), lastStableFCV);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Upgrading FCV to latest");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(configPrimary.getDB("admin"), latestFCV);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: true});

//
// Verify operations during setFeatureCompabitilityVersion use the correct format and that setFCV
// blocks behind in-progress shard collections on shard servers.
//

function runInProgressSetFCVTest(st, {initialFCV, desiredFCV}) {
    const pauseInSetFCVFailPointName = desiredFCV === lastStableFCV
        ? "pauseBeforeDowngradingConfigMetadata"
        : "pauseBeforeUpgradingConfigMetadata";

    clearRawMongoProgramOutput();
    checkFCV(configPrimary.getDB("admin"), initialFCV);

    // Pause setFCV to test the in-progress states.
    assert.commandWorked(configPrimary.adminCommand(
        {configureFailPoint: pauseInSetFCVFailPointName, mode: "alwaysOn"}));

    // Start and pause a shard collection, and verify that the setFCV blocks behind it.
    const shardCollDuringSetFCV = new Thread((host, ns) => {
        const mongosConn = new Mongo(host);
        return mongosConn.adminCommand({shardCollection: ns, key: {_id: 1}});
    }, st.s.host, dbName + ".setFCVTo" + desiredFCV);
    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        {configureFailPoint: "pauseShardCollectionBeforeReturning", mode: "alwaysOn"}));
    shardCollDuringSetFCV.start();
    waitForFailpoint("Hit pauseShardCollectionBeforeReturning", 1 /* numTimes */);

    // Assert setFCV can't hit the failpoint until the shard collection completes.
    const changeFCV = new Thread((host, fcv) => {
        const mongosConn = new Mongo(host);
        return mongosConn.adminCommand({setFeatureCompatibilityVersion: fcv});
    }, st.s.host, desiredFCV);
    changeFCV.start();
    assert.throws(() => {
        waitForFailpoint("Hit " + pauseInSetFCVFailPointName, 1 /* numTimes */, 3000 /* timeout */);
    });

    // Unpause the shard collection and wait for setFCV to reach the failpoint.
    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        {configureFailPoint: "pauseShardCollectionBeforeReturning", mode: "off"}));
    shardCollDuringSetFCV.join();
    waitForFailpoint("Hit " + pauseInSetFCVFailPointName, 1 /* numTimes */);

    // Verify behavior while setFCV is in progress.
    verifyChunkOperationsFailDuringSetFCV(st, chunkNs);
    verifyZoneOperationsSucceedDuringSetFCV(st, zoneNs);
    testCRUDOperations(st, chunkNs);
    testCRUDOperations(st, zoneNs);

    // A collection can still be sharded during setFCV and should write chunks correctly.
    verifyInitialChunks(
        st, dbName + ".newCollDuringFCV" + desiredFCV, {expectNewFormat: desiredFCV === latestFCV});

    // Unset the setFCV failpoint and allow setFCV to finish.
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: pauseInSetFCVFailPointName, mode: "off"}));
    changeFCV.join();
    assert.commandWorked(changeFCV.returnData());
    checkFCV(configPrimary.getDB("admin"), desiredFCV);

    verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: desiredFCV === latestFCV});
}

runInProgressSetFCVTest(st, {initialFCV: latestFCV, desiredFCV: lastStableFCV});
runInProgressSetFCVTest(st, {initialFCV: lastStableFCV, desiredFCV: latestFCV});

//
// Test setFCV with many chunks and tags.
//

// Set up collections with the same number of chunks and zones as the batch limit for the
// transactions used to modify chunks and zones documents and with more than the limit to verify the
// batching logic in both cases.
const txnBatchSize = 100;
setUpCollectionWithManyChunksAndZones(
    st, dbName + ".many_at_batch_size", txnBatchSize /* numChunks */, txnBatchSize /* numZones */);
setUpCollectionWithManyChunksAndZones(st,
                                      dbName + ".many_over_batch_size",
                                      txnBatchSize + 5 /* numChunks */,
                                      txnBatchSize + 5 /* numZones */);

checkFCV(configPrimary.getDB("admin"), latestFCV);

verifyChunks(st, {expectNewFormat: true});

jsTestLog("Downgrading FCV to last stable with many chunks and zones");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(configPrimary.getDB("admin"), lastStableFCV);

verifyChunks(st, {expectNewFormat: false});

jsTestLog("Upgrading FCV to latest with many chunks and zones");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(configPrimary.getDB("admin"), latestFCV);

verifyChunks(st, {expectNewFormat: true});

st.stop();
}());
