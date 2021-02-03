// Tests that the ShardingTaskExecutorPoolReplicaSetMatchingPolicy is correctly set when the default
// "automatic" value is used for the ShardingTaskExecutorPoolReplicaSetMatching parameter (on
// mongos, "matchPrimaryNode" should be set; on mongod, the policy should be "disabled").
//
// @tags: [sets_replica_set_matching_strategy]

(function() {
"use strict";

// Helper function to check the matching policy of a node, given the output of the connPoolStats
// command run against the node and the expected policy.
function checkSTEPReplicaSetMatchingPolicy(connPoolStats, expectedPolicy) {
    assert("replicaSetMatchingStrategy" in connPoolStats);
    let nodePolicy = connPoolStats["replicaSetMatchingStrategy"];
    assert.eq(nodePolicy, expectedPolicy);
}

// Helper function that checks, given a connection to a mongo{s, d}, that the output value returned
// by the "getParameter" command for the parameter "ShardingTaskExecutorPoolReplicaSetMatching"
// is "automatic".
function checkGetParameterOutputIsAuto(dbConn) {
    const getParameterCommand = {getParameter: 1};
    getParameterCommand["ShardingTaskExecutorPoolReplicaSetMatching"] = 1;
    const result = assert.commandWorked(dbConn.adminCommand(getParameterCommand));
    const value = result["ShardingTaskExecutorPoolReplicaSetMatching"];
    assert.eq(value, "automatic");
}

// Setup test fixture; get connection to mongos and each mongod.
let paramsDoc = {
    mongosOptions: {setParameter: {ShardingTaskExecutorPoolReplicaSetMatching: "automatic"}},
    shardOptions: {setParameter: {ShardingTaskExecutorPoolReplicaSetMatching: "automatic"}}
};
let st = new ShardingTest({shards: 2, mongos: 1, other: paramsDoc});
let mongos = st.s;
let mongod = st.shard0;
let otherMongod = st.shard1;

// Check that each connection currently has policy value "automatic"
checkGetParameterOutputIsAuto(mongos);
checkGetParameterOutputIsAuto(mongod);
checkGetParameterOutputIsAuto(otherMongod);

// Build connPoolStats command for each mongod and mongos; ensure they run without error.
let mongosStats = mongos.adminCommand({connPoolStats: 1});
let mongodStats = mongod.adminCommand({connPoolStats: 1});
let otherMongodStats = otherMongod.adminCommand({connPoolStats: 1});
assert.commandWorked(mongosStats);
assert.commandWorked(mongodStats);
assert.commandWorked(otherMongodStats);

// Ensure the CPS contain the correct output (described in header comment for this test).
checkSTEPReplicaSetMatchingPolicy(mongosStats, "matchPrimaryNode");
checkSTEPReplicaSetMatchingPolicy(mongodStats, "disabled");
checkSTEPReplicaSetMatchingPolicy(otherMongodStats, "disabled");
st.stop();
})();
