/**
 * Test the configureCollectionBalancing command and balancerCollectionStatus command
 *
 * @tags: [
 *  requires_fcv_53,
 *  featureFlagPerCollBalancingSettings,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/sharding/autosplit_include.js');
load("jstests/sharding/libs/find_chunks_util.js");

Random.setRandomSeed();

const st = new ShardingTest({
    mongos: 1,
    shards: 3,
    other: {
        enableBalancer: true,
        enableAutoSplit: true,
        // Set global max chunk size to 1MB
        chunkSize: 1,
        configOptions: {setParameter: {logComponentVerbosity: tojson({sharding: {verbosity: 2}})}},
    }
});

// setup the database for the test
assert.commandWorked(st.s.adminCommand({enableSharding: 'db'}));
const db = st.getDB('db');
const collName = 'testColl';
let collCounter = 0;

// Shorten time between balancer rounds for faster initial balancing
st.forEachConfigServer((conn) => {
    conn.adminCommand({
        configureFailPoint: 'overrideBalanceRoundInterval',
        mode: 'alwaysOn',
        data: {intervalMs: 200}
    });
});

const chunkSize = 2;
const bigString = "X".repeat(32 * 1024);  // 32 KB

function waitForBalanced(ns) {
    assert.soon(function() {
        st.awaitBalancerRound();
        let balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: ns}));
        return balancerStatus.balancerCompliant;
    });
}

function setupCollection() {
    st.startBalancer();
    const coll = db[collName + collCounter];
    collCounter++;
    const fullNs = coll.getFullName();
    assert.commandWorked(st.s.adminCommand({shardCollection: fullNs, key: {key: 1}}));

    // "startBalancer" only guarantee that all the shards will EVENTUALLY enable the autosplitter.
    // In order to ensure that all the shards have ALREADY enabled the autosplitter, we use the
    // configureCollectionBalancing command instead.
    assert.commandWorked(
        st.s.adminCommand({configureCollectionBalancing: fullNs, enableAutoSplitter: true}));

    for (let i = 0; i < 250; i++) {
        assert.commandWorked(coll.insert({key: Random.randInt(1000) - 500, str: bigString}));
        waitForOngoingChunkSplits(st);
    }
    const numChunksPrev = findChunksUtil.countChunksForNs(st.config, fullNs);
    jsTest.log("Collection " + fullNs + ", number of chunks before merging: " + numChunksPrev);

    jsTest.log("Balance cluster before beginning defragmentation");
    waitForBalanced(fullNs);

    return fullNs;
}

function setFailPointOnConfigNodes(failpoint, mode) {
    st.forEachConfigServer((config) => {
        assert.commandWorked(config.adminCommand({configureFailPoint: failpoint, mode: mode}));
    });
}

function waitForAnyFailpointOnConfigNodes(timesEntered) {
    assert.soon(function() {
        let hitFailpoint = false;
        st.forEachConfigServer((config) => {
            let res = assert.commandWorkedOrFailedWithCode(config.adminCommand({
                waitForFailPoint: "beforeTransitioningDefragmentationPhase",
                timesEntered: timesEntered + 1,
                maxTimeMS: kDefaultWaitForFailPointTimeout / 10
            }),
                                                           ErrorCodes.MaxTimeMSExpired);
            hitFailpoint = hitFailpoint || res["ok"] === 1;
        });
        return hitFailpoint;
    });
}

// Setup collection for first tests
const coll1 = setupCollection();

jsTest.log("Begin and end defragmentation with balancer off.");
{
    st.stopBalancer();
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        chunkSize: chunkSize,
    }));
    let beforeStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        chunkSize: chunkSize,
    }));
    let afterStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.eq(afterStatus.balancerCompliant, true);
}

jsTest.log("Begin and end defragmentation with balancer on");
{
    st.startBalancer();
    // Allow the first phase transition to build the initial defragmentation state
    setFailPointOnConfigNodes("beforeTransitioningDefragmentationPhase", {skip: 1});
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        chunkSize: chunkSize,
    }));
    st.awaitBalancerRound();
    let beforeStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        chunkSize: chunkSize,
    }));
    setFailPointOnConfigNodes("beforeTransitioningDefragmentationPhase", "off");
    st.awaitBalancerRound();
    let afterStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.neq(afterStatus.firstComplianceViolation, 'chunksMerging');
    st.stopBalancer();
}

const coll2 = setupCollection();
jsTest.log("Begin defragmentation with balancer off, end with it on");
{
    // Allow the first phase transition to build the initial defragmentation state
    setFailPointOnConfigNodes("beforeTransitioningDefragmentationPhase", {skip: 1});
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll2,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        chunkSize: chunkSize,
    }));
    st.startBalancer();
    st.awaitBalancerRound();
    let beforeStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll2}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll2,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        chunkSize: chunkSize,
    }));
    setFailPointOnConfigNodes("beforeTransitioningDefragmentationPhase", "off");
    st.awaitBalancerRound();
    let afterStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll2}));
    assert.neq(afterStatus.firstComplianceViolation, 'chunksMerging');
    st.stopBalancer();
}

const coll3 = setupCollection();
jsTest.log("Balancer on, begin defragmentation and let it complete");
{
    // Reset collection before starting
    const numChunksPrev = findChunksUtil.countChunksForNs(st.config, coll3);
    // Pause after phase 1 completes to check merging succeeded
    setFailPointOnConfigNodes("beforeTransitioningDefragmentationPhase", {skip: 1});
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll3,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        chunkSize: chunkSize,
    }));
    st.startBalancer();
    // Wait for phase 1 to complete
    waitForAnyFailpointOnConfigNodes(0);
    const numChunksPost = findChunksUtil.countChunksForNs(st.config, coll3);
    jsTest.log("Number of chunks after merging " + numChunksPost);
    assert.lte(numChunksPost, numChunksPrev);
    // Turn fail point off, let phase 3 run and complete
    setFailPointOnConfigNodes("beforeTransitioningDefragmentationPhase", "off");
    assert.soon(function() {
        st.awaitBalancerRound();
        let balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll3}));
        return balancerStatus.firstComplianceViolation != 'chunksMerging';
    });
    st.stopBalancer();
    const numChunksEnd = findChunksUtil.countChunksForNs(st.config, coll3);
    jsTest.log("Number of chunks after splitting " + numChunksEnd);
    assert.gte(numChunksEnd, numChunksPost);
}

const collection4 = db[collName + collCounter];
const coll4 = collection4.getFullName();
collCounter++;
assert.commandWorked(st.s.adminCommand({shardCollection: coll4, key: {key: 1}}));
jsTest.log("Changed uuid causes defragmentation to restart");
{
    // Create two chunks on shard0
    collection4.insertOne({key: -1, key2: -1});
    collection4.insertOne({key: 1, key2: 1});
    assert.commandWorked(db.adminCommand({split: coll4, middle: {key: 1}}));
    // Pause defragmentation after initialization but before phase 1 runs
    setFailPointOnConfigNodes("afterBuildingNextDefragmentationPhase", "alwaysOn");
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll4,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        chunkSize: chunkSize,
    }));
    st.startBalancer();
    // Reshard collection
    assert.commandWorked(db.adminCommand({reshardCollection: coll4, key: {key2: 1}}));
    assert.commandWorked(
        db.adminCommand({moveChunk: coll4, find: {key2: MinKey}, to: st.shard0.shardName}));
    assert.commandWorked(
        db.adminCommand({moveChunk: coll4, find: {key2: 1}, to: st.shard0.shardName}));
    // Let defragementation run
    setFailPointOnConfigNodes("afterBuildingNextDefragmentationPhase", "off");
    assert.soon(function() {
        st.awaitBalancerRound();
        let balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll4}));
        return balancerStatus.firstComplianceViolation != 'chunksMerging';
    });
    st.stopBalancer();
    // Ensure the defragmentation succeeded
    const numChunksEnd = findChunksUtil.countChunksForNs(st.config, coll4);
    assert.eq(numChunksEnd, 1);
}

const collection5 = db[collName + collCounter];
const coll5 = collection5.getFullName();
collCounter++;
assert.commandWorked(st.s.adminCommand({shardCollection: coll5, key: {key: 1}}));
jsTest.log("Refined shard key causes defragmentation to restart");
{
    // Create two chunks on shard0
    collection5.insertOne({key: -1, key2: -1});
    collection5.insertOne({key: 1, key2: 1});
    assert.commandWorked(db.adminCommand({split: coll5, middle: {key: 1}}));
    // Pause defragmentation after initialization but before phase 1 runs
    setFailPointOnConfigNodes("afterBuildingNextDefragmentationPhase", "alwaysOn");
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll5,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        chunkSize: chunkSize,
    }));
    st.startBalancer();
    // Refine shard key - shouldn't change uuid
    assert.commandWorked(collection5.createIndex({key: 1, key2: 1}));
    assert.commandWorked(
        db.adminCommand({refineCollectionShardKey: coll5, key: {key: 1, key2: 1}}));
    // Let defragementation run
    setFailPointOnConfigNodes("afterBuildingNextDefragmentationPhase", "off");
    assert.soon(function() {
        st.awaitBalancerRound();
        let balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll5}));
        return balancerStatus.firstComplianceViolation != 'chunksMerging';
    });
    st.stopBalancer();
    // Ensure the defragmentation succeeded
    const numChunksEnd = findChunksUtil.countChunksForNs(st.config, coll5);
    assert.eq(numChunksEnd, 1);
}

st.stop();
})();
