/**
 * Test that movePrimary sets valid block timestamp if a failover occurs before persisting it.
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
const config = st.config;

const dbName = 'test_db';
const collName = 'test_coll';
const collNS = dbName + '.' + collName;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));
assert.commandWorked(mongos.getCollection(collNS).insert({value: 1}));
assert.commandWorked(mongos.getCollection(collNS).insert({value: 2}));

const fp = configureFailPoint(oldDonorPrimary, "pauseBeforeMovePrimaryDonorPersistsBlockTimestamp");

const joinMovePrimary = startParallelShell(
    funWithArgs(function(dbName, toShard) {
        assert.commandWorked(db.adminCommand({movePrimary: dbName, to: toShard}));
    }, dbName, shard1.shardName), mongos.port);

fp.wait();
st.rs0.getPrimary().adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: 1});
fp.off();
st.rs0.awaitNodesAgreeOnPrimary();
joinMovePrimary();

st.stop();
})();
