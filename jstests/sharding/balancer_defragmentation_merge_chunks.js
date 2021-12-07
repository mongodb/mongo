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

var st = new ShardingTest({
    mongos: 1,
    shards: 3,
    config: 1,
    other: {
        enableBalancer: true,
        enableAutoSplit: true,
        configOptions: {setParameter: {logComponentVerbosity: tojson({sharding: {verbosity: 2}})}},
    }
});

// setup the database for the test
assert.commandWorked(st.s.adminCommand({enableSharding: 'db'}));
var db = st.getDB('db');
var coll = db['test'];
var fullNs = coll.getFullName();
var configPrimary = st.configRS.getPrimary();

const defaultChunkSize = 2;
const bigString = "X".repeat(32 * 1024);  // 32 KB
assert.commandWorked(st.s.adminCommand({shardCollection: fullNs, key: {key: 1}}));

// TODO (SERVER-61848) remove this once the chunk size setting works
let configDB = st.s.getDB('config');
assert.commandWorked(configDB["settings"].insertOne({_id: "chunksize", value: 1}));

var bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 12 * 128; i++) {
    bulk.insert({key: i, str: bigString});
}
assert.commandWorked(bulk.execute());
waitForOngoingChunkSplits(st);
const numChunksPrev = findChunksUtil.countChunksForNs(st.config, fullNs);
jsTest.log("Number of chunks before merging " + numChunksPrev);

jsTest.log("Balance cluster before beginning defragmentation");

function waitForBalanced() {
    assert.soon(function() {
        st.awaitBalancerRound();
        var balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: fullNs}));
        return balancerStatus.balancerCompliant;
    });
}

waitForBalanced();

jsTest.log("Begin and end defragmentation with balancer off.");
{
    st.stopBalancer();
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    // Defragmentation should not start with the balancer stopped
    var balancerStatus =
        assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: fullNs}));
    assert.eq(balancerStatus.balancerCompliant, true);
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        defaultChunkSize: defaultChunkSize,
    }));
    st.startBalancer();
    st.awaitBalancerRound();
    assert.eq(balancerStatus.balancerCompliant, true);
    st.stopBalancer();
}

jsTest.log("Begin and end defragmentation with balancer on");
{
    st.startBalancer();
    var phaseTransitionFailpoint = configureFailPoint(configPrimary, "skipPhaseTransition");
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    st.awaitBalancerRound();
    var currStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: fullNs}));
    assert.eq(currStatus.balancerCompliant, false);
    assert.eq(currStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        defaultChunkSize: defaultChunkSize,
    }));
    st.awaitBalancerRound();
    assert.eq(balancerStatus.balancerCompliant, true);
    st.stopBalancer();
    phaseTransitionFailpoint.off();
}

jsTest.log("Begin defragmentation with balancer off, end with it on");
{
    var phaseTransitionFailpoint = configureFailPoint(configPrimary, "skipPhaseTransition");
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    st.startBalancer();
    st.awaitBalancerRound();
    var currStatus = assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: fullNs}));
    assert.eq(currStatus.balancerCompliant, false);
    assert.eq(currStatus.firstComplianceViolation, 'chunksMerging');
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: false,
        defaultChunkSize: defaultChunkSize,
    }));
    st.awaitBalancerRound();
    assert.eq(balancerStatus.balancerCompliant, true);
    st.stopBalancer();
    phaseTransitionFailpoint.off();
}

jsTest.log("Balancer on, begin defragmentation and let it complete");
{
    // Reset collection before starting
    st.startBalancer();
    waitForBalanced();
    const numChunksPrev = findChunksUtil.countChunksForNs(st.config, fullNs);
    jsTest.log("Number of chunks before merging " + numChunksPrev);
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    assert.soon(function() {
        st.awaitBalancerRound();
        var balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: fullNs}));
        return balancerStatus.firstComplianceViolation != 'chunksMerging';
    });
    st.stopBalancer();
    const numChunksPost = findChunksUtil.countChunksForNs(st.config, fullNs);
    jsTest.log("Number of chunks after merging " + numChunksPost);
    assert.lt(numChunksPost, numChunksPrev);
}

st.stop();
})();
