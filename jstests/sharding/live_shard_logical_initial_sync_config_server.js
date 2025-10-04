/**
 * Tests that sharding state is properly initialized on new config members that were added into live
 * config server replica sets using logical initial sync.
 *
 * We control our own failovers, and we also need the RSM to react reasonably quickly to those.
 * @tags: [does_not_support_stepdowns, requires_streamable_rsm]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardingStateTest} from "jstests/sharding/libs/sharding_state_test.js";

const st = new ShardingTest({
    config: 1,
    shards: {rs0: {nodes: 1}},
});
const configRS = st.configRS;

let newNode;
if (TestData.configShard) {
    newNode = ShardingStateTest.addReplSetNode({
        replSet: configRS,
        serverTypeFlag: "configsvr",
        newNodeParams: "featureFlagTransitionToCatalogShard=true",
    });
} else {
    newNode = ShardingStateTest.addReplSetNode({replSet: configRS, serverTypeFlag: "configsvr"});
}

jsTestLog("Checking sharding state before failover.");
ShardingStateTest.checkShardingState(st);

jsTestLog("Checking sharding state after failover.");
ShardingStateTest.failoverToMember(configRS, newNode);
ShardingStateTest.checkShardingState(st);

st.stop();
