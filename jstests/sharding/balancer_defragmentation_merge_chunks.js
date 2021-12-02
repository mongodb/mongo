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

var st = new ShardingTest(
    {mongos: 1, shards: 3, config: 1, other: {enableBalancer: true, enableAutoSplit: true}});

// setup the database for the test
assert.commandWorked(st.s.adminCommand({enableSharding: 'db'}));
var db = st.getDB('db');
var coll = db['test'];
var fullNs = coll.getFullName();
var configPrimary = st.configRS.getPrimary();

const defaultChunkSize = 2 * 1024 * 1024;
const bigString = "X".repeat(32 * 1024);  // 32 KB
assert.commandWorked(st.s.adminCommand({shardCollection: fullNs, key: {key: 1}}));

var bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 32 * 128; i++) {
    bulk.insert({key: i, str: bigString});
}
assert.commandWorked(bulk.execute());
waitForOngoingChunkSplits(st);

jsTest.log("Balance cluster before beginning defragmentation");

function waitForBalanced() {
    assert.soon(function() {
        st.awaitBalancerRound();
        balancerStatus =
            assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: fullNs}));
        return balancerStatus.balancerCompliant;
    });
    jsTest.log("Balancer status of " + fullNs + " : \n" + tojson(balancerStatus));
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
    st.startBalancer();
    assert.commandWorked(st.s.adminCommand({
        configureCollectionAutoSplitter: fullNs,
        enableAutoSplitter: false,
        balancerShouldMergeChunks: true,
        defaultChunkSize: defaultChunkSize,
    }));
    waitForBalanced();
}

st.stop();
})();
