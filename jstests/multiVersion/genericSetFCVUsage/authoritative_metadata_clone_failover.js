/*
 * Validates that the authoritative metadata cloning DDL can resume after a failover,
 and that it will only clone the remaining collections (rather than starting from scratch).
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, config: 1, rs: {nodes: 2}});
if (lastLTSFCV !== "8.0" || !FeatureFlagUtil.isPresentAndEnabled(st.s, "AuthoritativeShardsCRUD")) {
    jsTestLog("Skipping: not running across the FCV transition to Authoritative Shards");
    st.stop();
    quit();
}

// Create a few tracked collections.
const dbName = "test";
const collNames = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k"];
for (const coll of collNames) {
    assert.commandWorked(st.s.adminCommand({shardCollection: `${dbName}.${coll}`, key: {_id: 1}}));
}

// Leave the cluster in the transitional kUpgrading FCV so the cloning DDL is allowed to run.
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);
configureFailPoint(st.configRS.getPrimary(), "failUpgrading", {}, {times: 1});
assert.commandFailedWithCode(
    st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    549180,
);

// Let the coordinator clone 7 of the collections, then fail over to another primary.
const cloneCollFp = configureFailPoint(
    st.rs0.getPrimary(),
    "hangBeforeCloningCollectionCloneAuthoritativeMetadataDDL",
    {} /* data */,
    {skip: 7},
);

const awaitCloneOnOriginalPrimary = startParallelShell(() => {
    assert.commandFailedWithCode(
        db.adminCommand({_shardsvrCloneAuthoritativeMetadata: 1}),
        ErrorCodes.InterruptedDueToReplStateChange,
    );
}, st.rs0.getPrimary().port);

cloneCollFp.wait();

const originalPrimary = st.rs0.getPrimary();
const newPrimary = st.rs0.getSecondary();

st.rs0.awaitReplication();
st.rs0.stepUp(newPrimary);
cloneCollFp.off();

awaitCloneOnOriginalPrimary();

// Wait for the clone to finish on the new primary.
assert.commandWorked(newPrimary.adminCommand({_shardsvrJoinDDLCoordinators: 1}));

// The cloning finished, so every collection should be authoritative in the shard catalog.
assert.eq(
    collNames.length,
    st.shard0.getDB("config").shard.catalog.collections.count({
        _id: {$in: collNames.map((coll) => `${dbName}.${coll}`)},
    }),
);

// We should only have cloned each tracked collection once.
const countClonedByNode = (node) =>
    node.getDB("admin").serverStatus().shardingStatistics.collectionShardingMetadataStatistics
        .countLocalCollectionMetadataClones;
assert.eq(countClonedByNode(originalPrimary) + countClonedByNode(newPrimary), collNames.length);

st.stop();
