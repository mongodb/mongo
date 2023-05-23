/**
 * Test that movePrimary coordinator recovers and cleans up the donor after a failover when it is
 * already aborted.
 *
 *  @tags: [
 *    featureFlagOnlineMovePrimaryLifecycle
 * ]
 */
(function() {
'use strict';
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}});

if (!FeatureFlagUtil.isEnabled(st.s.getDB(jsTestName()), "OnlineMovePrimaryLifecycle")) {
    jsTestLog('Skipping test because the featureFlagOnlineMovePrimaryLifecycle is disabled.');
    st.stop();
    return;
}

const mongos = st.s0;
const shard0 = st.shard0;
const oldDonorPrimary = st.rs0.getPrimary();
const shard1 = st.shard1;

const dbName = 'test_db';
const collName = 'test_coll';
const collNS = dbName + '.' + collName;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));
assert.commandWorked(mongos.getCollection(collNS).insert({value: 1}));
assert.commandWorked(mongos.getCollection(collNS).insert({value: 2}));

const donorStartedCloningFp = configureFailPoint(oldDonorPrimary,
                                                 "pauseDuringMovePrimaryDonorStateTransition",
                                                 {progress: "after", state: "cloning"});

// Run movePrimary and wait for MovePrimaryDonor to start.
const joinMovePrimary = startParallelShell(
    funWithArgs(function(dbName, toShard) {
        assert.commandFailed(db.adminCommand({movePrimary: dbName, to: toShard}));
    }, dbName, shard1.shardName), mongos.port);

donorStartedCloningFp.wait();

// Trigger a failover. The MovePrimaryCoordinator will abort on step up. Make sure it does not clean
// up the donor yet.
const pauseCoordinatorFps = new Map();
st.rs0.nodes.map(node => pauseCoordinatorFps.put(
                     node, configureFailPoint(node, "movePrimaryCoordinatorHangBeforeCleaningUp")));
st.rs0.getPrimary().adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: 1});
donorStartedCloningFp.off();
st.rs0.awaitNodesAgreeOnPrimary();

// TODO SERVER-77115: Investigate why test times out if this sleep is removed.
sleep(5000);

// Trigger another failover when 1. the MovePrimaryCoordinator is already aborted and 2. the
// MovePrimaryDonor is still alive. This is the case this test is trying to set up.
pauseCoordinatorFps.get(st.rs0.getPrimary()).wait();
st.rs0.getPrimary().adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: 1});
st.rs0.awaitNodesAgreeOnPrimary();
pauseCoordinatorFps.values().map(fp => fp.off());
joinMovePrimary();

// Verify that the MovePrimaryCoordinator has cleaned up the MovePrimaryDonor.
assert.eq([], shard0.getDB("config").movePrimaryDonors.find({}).toArray());

st.stop();
})();
