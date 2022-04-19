/**
 * Tests that sharding state is properly initialized on new config members that were added into live
 * config server replica sets using logical initial sync.
 */

(function() {
"use strict";

load("jstests/sharding/libs/sharding_state_test.js");

const st = new ShardingTest({
    config: 1,
    shards: {rs0: {nodes: 1}},
});
const configRS = st.configRS;

const newNode = ShardingStateTest.addReplSetNode({replSet: configRS, serverTypeFlag: "configsvr"});

jsTestLog("Checking sharding state before failover.");
ShardingStateTest.checkShardingState(st);

jsTestLog("Checking sharding state after failover.");
ShardingStateTest.failoverToMember(configRS, newNode);
ShardingStateTest.checkShardingState(st);

st.stop();
})();
