/**
 * Tests that a drop can't happen while moveCollection is in progress.
 * @tags: [
 *  featureFlagRecoverableShardsvrReshardCollectionCoordinator,
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  multiversion_incompatible,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

var st = new ShardingTest({
    shards: {rs0: {nodes: 2}, rs1: {nodes: 1}},
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

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.getDB(dbName).runCommand({createUnsplittableCollection: collName}));

const reshardingPauseCoordinatorBeforeInitializingFailpoint =
    configureFailPoint(st.configRS.getPrimary(), "reshardingPauseCoordinatorBeforeInitializing");

assert.commandFailedWithCode(
    db.adminCommand({moveCollection: ns, toShard: st.shard1.shardName, maxTimeMS: 1000}),
    ErrorCodes.MaxTimeMSExpired);

// Wait for resharding to start running on the configsvr
reshardingPauseCoordinatorBeforeInitializingFailpoint.wait();

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
reshardingPauseCoordinatorBeforeInitializingFailpoint.off();
assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));

// Now the drop can complete
assert.commandWorked(db.runCommand({drop: collName}));

st.stop();
