/**
 * Test the setCollectionAutosplitter command and balancerCollectionStatus command
 *
 * @tags: [
 *  requires_fcv_51,
 *  featureFlagPerCollectionAutoSplitter,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/sharding/autosplit_include.js');
load("jstests/sharding/libs/find_chunks_util.js");

const st = new ShardingTest({
    mongos: 1,
    shards: 3,
    other: {
        enableBalancer: true,
        enableAutoSplit: true,
        configOptions: {setParameter: {logComponentVerbosity: tojson({sharding: {verbosity: 2}})}},
    }
});

// setup the database for the test
assert.commandWorked(st.s.adminCommand({enableSharding: 'db'}));
const db = st.getDB('db');
const collName = 'testColl';
let collCounter = 0;

// Set global max chunk size to 1MB
assert.soonNoExcept(() => {
    let configDB = st.s.getDB('config');
    assert.commandWorked(
        configDB["settings"].update({_id: "chunksize"}, {$set: {value: 1}}, {upsert: true}));
    return true;
});

// Shorten time between balancer rounds for faster initial balancing
st.forEachConfigServer((conn) => {
    conn.adminCommand({
        configureFailPoint: 'overrideBalanceRoundInterval',
        mode: 'alwaysOn',
        data: {intervalMs: 200}
    });
});

const defaultChunkSize = 2;
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

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 12 * 128; i++) {
        bulk.insert({key: i, str: bigString});
    }
    assert.commandWorked(bulk.execute());
    waitForOngoingChunkSplits(st);
    const numChunksPrev = findChunksUtil.countChunksForNs(st.config, fullNs);
    jsTest.log("Collection " + fullNs + ", number of chunks before merging: " + numChunksPrev);

    jsTest.log("Balance cluster before beginning defragmentation");
    waitForBalanced(fullNs);

    return fullNs;
}

function setFailPointOnConfigNodes(mode) {
    st.forEachConfigServer((config) => {
        assert.commandWorked(config.adminCommand(
            {configureFailPoint: "beforeTransitioningDefragmentationPhase", mode: mode}));
    });
}

// Setup collection for first tests
const coll1 = setupCollection();

jsTest.log("Begin and end defragmentation with balancer off.");
{
    st.stopBalancer();
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    let beforeStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        defaultChunkSize: defaultChunkSize,
    }));
    let afterStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.eq(afterStatus.balancerCompliant, true);
}

jsTest.log("Begin and end defragmentation with balancer on");
{
    st.startBalancer();
    // Allow the first phase transition to build the initial defragmentation state
    setFailPointOnConfigNodes({skip: 1});
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    st.awaitBalancerRound();
    let beforeStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: coll1,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        defaultChunkSize: defaultChunkSize,
    }));
    setFailPointOnConfigNodes("off");
    st.awaitBalancerRound();
    let afterStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll1}));
    assert.neq(afterStatus.firstComplianceViolation, 'chunksMerging');
    st.stopBalancer();
}

const coll2 = setupCollection();
jsTest.log("Begin defragmentation with balancer off, end with it on");
{
    // Allow the first phase transition to build the initial defragmentation state
    setFailPointOnConfigNodes({skip: 1});
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: coll2,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    st.startBalancer();
    st.awaitBalancerRound();
    let beforeStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll2}));
    assert.eq(beforeStatus.balancerCompliant, false);
    assert.eq(beforeStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: coll2,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        defaultChunkSize: defaultChunkSize,
    }));
    setFailPointOnConfigNodes("off");
    st.awaitBalancerRound();
    let afterStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll2}));
    assert.neq(afterStatus.firstComplianceViolation, 'chunksMerging');
    st.stopBalancer();
}

const coll3 = setupCollection();
jsTest.log("Balancer on, begin defragmentation and let it complete");
{
    const numChunksPrev = findChunksUtil.countChunksForNs(st.config, coll3);
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: coll3,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    assert.soon(function() {
        st.awaitBalancerRound();
        let balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: coll3}));
        return balancerStatus.firstComplianceViolation != 'chunksMerging';
    });
    st.stopBalancer();
    const numChunksPost = findChunksUtil.countChunksForNs(st.config, coll3);
    jsTest.log("Number of chunks after merging " + numChunksPost);
    assert.lt(numChunksPost, numChunksPrev);
}

st.stop();
})();
