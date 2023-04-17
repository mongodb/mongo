/**
 * Tests that a drop can't happen while resharding is in progress.
 * @tags: [
 *  requires_fcv_53,
 *  featureFlagRecoverableShardsvrReshardCollectionCoordinator,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

var st = new ShardingTest({
    shards: {rs0: {nodes: 2}},
    config: TestData.configShard ? 2 : 1,
    mongos: 1,
    other: {
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000}}
    }
});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const db = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

const reshardingPauseBeforeInsertCoordinatorDocFailpoint =
    configureFailPoint(st.configRS.getPrimary(), "pauseBeforeInsertCoordinatorDoc");

assert.commandFailedWithCode(
    db.adminCommand({reshardCollection: ns, key: {newKey: 1}, maxTimeMS: 1000}),
    ErrorCodes.MaxTimeMSExpired);

// Wait for resharding to start running on the configsvr
reshardingPauseBeforeInsertCoordinatorDocFailpoint.wait();

// Drop cannot progress while resharding is in progress
assert.commandFailedWithCode(db.runCommand({drop: collName, maxTimeMS: 5000}),
                             ErrorCodes.MaxTimeMSExpired);

// Stepdown the DB primary shard
const shard0Primary = st.rs0.getPrimary();
assert.commandWorked(
    shard0Primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
st.rs0.awaitNodesAgreeOnPrimary();

// Even after stepdown, drop cannot progress due to the in-progress resharding
assert.commandFailedWithCode(db.runCommand({drop: collName, maxTimeMS: 5000}),
                             ErrorCodes.MaxTimeMSExpired);

// Finish resharding
reshardingPauseBeforeInsertCoordinatorDocFailpoint.off();
assert.commandWorked(db.adminCommand({reshardCollection: ns, key: {newKey: 1}}));

// Now the drop can complete
assert.commandWorked(db.runCommand({drop: collName}));

st.stop();
})();
