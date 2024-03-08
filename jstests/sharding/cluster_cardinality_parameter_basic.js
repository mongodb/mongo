/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" has the correct value
 * after addShard and removeShard commands.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkClusterParameter} from "jstests/sharding/libs/cluster_cardinality_parameter_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

const st = new ShardingTest({shards: 1});

jsTest.log("Checking the cluster parameter while the cluster contains one shard");
// There is only one shard in the cluster, so the cluster parameter should be false.
checkClusterParameter(st.configRS, false);
checkClusterParameter(st.rs0, false);

const shard1Name = "shard1";
const shard1Rst = new ReplSetTest({name: shard1Name, nodes: 1});
shard1Rst.startSet({shardsvr: ""});
shard1Rst.initiate();
assert.commandWorked(st.s.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

jsTest.log("Checking the cluster parameter while the cluster contains two shards");
// The addShard command should set the cluster parameter to true.
checkClusterParameter(st.configRS, true);
checkClusterParameter(st.rs0, true);
checkClusterParameter(shard1Rst, true);

removeShard(st, shard1Name);

jsTest.log("Checking the cluster parameter while the cluster contains one shard again");
// The removeShard command should set to cluster parameter to false if the config server primary's
// binVersion is 7.3 or the replica set endpoint feature flag is enabled.
let binVersion =
    assert.commandWorked(st.configRS.getPrimary().adminCommand({serverStatus: 1})).version;
let rsEndpointEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.configRS.getPrimary(), "RSEndpointClusterCardinalityParameter");
const expectedHasTwoOrMoreShards =
    MongoRunner.compareBinVersions(binVersion, "7.3") != 0 && !rsEndpointEnabled;
checkClusterParameter(st.configRS, expectedHasTwoOrMoreShards);
checkClusterParameter(st.rs0, expectedHasTwoOrMoreShards);

assert.commandWorked(st.s.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

jsTest.log("Checking the cluster parameter while the cluster contains two shards again");
// The addShard command should set the cluster parameter to true again.
checkClusterParameter(st.configRS, true);
checkClusterParameter(st.rs0, true);
checkClusterParameter(shard1Rst, true);

const shard2Name = "shard2";
const shard2Rst = new ReplSetTest({name: shard2Name, nodes: 1});
shard2Rst.startSet({shardsvr: ""});
shard2Rst.initiate();
assert.commandWorked(st.s.adminCommand({addShard: shard2Rst.getURL(), name: shard2Name}));

jsTest.log("Checking the cluster parameter while the cluster contains three shards");
// The addShard command should not change the cluster parameter, i.e. it should remain true.
checkClusterParameter(st.configRS, true);
checkClusterParameter(st.rs0, true);
checkClusterParameter(shard1Rst, true);
checkClusterParameter(shard2Rst, true);

removeShard(st, shard2Name);

jsTest.log("Checking the cluster parameter while the cluster contains two shards again");
// The removeShard command should not change the cluster parameter, i.e. it should remain true.
checkClusterParameter(st.configRS, true);
checkClusterParameter(st.rs0, true);
checkClusterParameter(shard1Rst, true);

shard1Rst.stopSet();
shard2Rst.stopSet();
st.stop();
