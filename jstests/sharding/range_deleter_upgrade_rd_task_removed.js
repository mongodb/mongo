/**
 * Tests that setting orphan counters on upgrade is resilient to range deletions completing.
 *
 * @tags: [
 *  requires_fcv_60
 * ]
 */

(function() {
'use strict';
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const st = new ShardingTest({shards: 2, rs: {nodes: 1}, other: {enableBalancer: false}});

// Setup database and collection for test
const dbName = 'db';
const db = st.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const coll = db['test'];
const nss = coll.getFullName();
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));

let rangeDeletionFP = configureFailPoint(st.shard0, "pauseBeforeRemovingRangeDeletionTask");
let orphanCountSetterFP = configureFailPoint(st.shard0, "pauseBeforeSettingOrphanCountOnDocument");

jsTest.log(
    "Create a range deletion and wait for it to complete but pause before removing the task");
assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: st.shard1.shardName}));
rangeDeletionFP.wait();

jsTest.log("Begin FCV upgrade to 6.0 where orphan counts must be set");
const FCVUpgrade = startParallelShell(
    funWithArgs(function(fcv) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv}));
    }, latestFCV), st.s.port);

orphanCountSetterFP.wait();

jsTest.log("Release the range deletion failpoint and wait for the task to be removed");
rangeDeletionFP.off();
assert.soon(() => {
    let rangeDeletionTasks = st.rs0.getPrimary().getDB('config').rangeDeletions.find().toArray();
    return rangeDeletionTasks.length === 0;
});

jsTest.log("Release the upgrade task");
orphanCountSetterFP.off();
FCVUpgrade();

st.stop();
})();
