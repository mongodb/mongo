/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" has the correct value
 * after upgrade, downgrade, and addShard.
 *
 * @tags: [multiversion_incompatible, featureFlagClusterCardinalityParameter,
 * temporary_catalog_shard_incompatible]
 */

(function() {
'use strict';

load("jstests/sharding/libs/remove_shard_util.js");

const st = new ShardingTest({shards: 1, useHostname: false});

const additionalShard = new ReplSetTest({name: "additionalShard", host: 'localhost', nodes: 1});
additionalShard.startSet({shardsvr: ""});
additionalShard.initiate();

let checkClusterParameter = function(conn, expectedValue) {
    let resp = assert.commandWorked(
        conn.adminCommand({getClusterParameter: "shardedClusterCardinalityForDirectConns"}));
    assert.eq(resp.clusterParameters[0].hasTwoOrMoreShards, expectedValue);
};

// There is only one shard in the cluster, so the cluster parameter should be false
checkClusterParameter(st.configRS.getPrimary(), false);
checkClusterParameter(st.shard0, false);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandWorked(st.s.adminCommand({addShard: additionalShard.getURL(), name: "shard02"}));
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// There are two shards in the cluster while upgrading, so the cluster parameter should be true
checkClusterParameter(st.configRS.getPrimary(), true);
checkClusterParameter(st.shard0, true);
checkClusterParameter(additionalShard.getPrimary(), true);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
removeShard(st, "shard02");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// There is one shard in the cluster while upgrading, so the cluster parameter should be false
checkClusterParameter(st.configRS.getPrimary(), false);
checkClusterParameter(st.shard0, false);

assert.commandWorked(st.s.adminCommand({addShard: additionalShard.getURL(), name: "shard02"}));

// Since the feature flag is enabled, addShard should update the cluster parameter
checkClusterParameter(st.configRS.getPrimary(), true);
checkClusterParameter(st.shard0, true);
checkClusterParameter(additionalShard.getPrimary(), true);

removeShard(st, "shard02");

// Removing a shard while the feature flag is enabled shouldn't change the cluster parameter
checkClusterParameter(st.configRS.getPrimary(), true);
checkClusterParameter(st.shard0, true);

additionalShard.stopSet();
st.stop();
})();
