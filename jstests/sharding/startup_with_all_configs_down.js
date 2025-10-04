// Tests that mongos and shard mongods can both be started up successfully when there is no config
// server, and that they will wait until there is a config server online before handling any
// sharding operations.
//
// This test involves restarting a standalone shard, so cannot be run on ephemeral storage engines.
// A restarted standalone will lose all data when using an ephemeral storage engine.
// @tags: [requires_persistence]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// The following checks use connections to shards cached on the ShardingTest object, but this test
// restarts a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;

let st = new ShardingTest({
    shards: 2,
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

jsTestLog("Setting up initial data");
assert.commandWorked(st.s0.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));
for (let i = 0; i < 100; i++) {
    assert.commandWorked(st.s.getDB("test").foo.insert({_id: i}));
}

assert.commandWorked(st.s0.adminCommand({shardCollection: "test.foo", key: {_id: 1}}));
assert.commandWorked(st.s0.adminCommand({split: "test.foo", find: {_id: 50}}));
assert.commandWorked(st.s0.adminCommand({moveChunk: "test.foo", find: {_id: 75}, to: st.shard1.shardName}));

// Make sure the pre-existing mongos already has the routing information loaded into memory
assert.eq(100, st.s.getDB("test").foo.find().itcount());

jsTestLog("Shutting down all config servers");
st.configRS.nodes.forEach((config) => {
    st.stopConfigServer(config);
});

jsTestLog("Starting a new mongos when there are no config servers up");
let newMongosInfo = MongoRunner.runMongos({configdb: st._configDB, waitForConnect: false});
// The new mongos won't accept any new connections, but it should stay up and continue trying
// to contact the config servers to finish startup.
assert.throws(function () {
    new Mongo(newMongosInfo.host);
});

jsTestLog("Restarting a shard while there are no config servers up");
st.rs1.stopSet(undefined, true);
st.rs1.startSet({waitForConnect: false}, true);

jsTestLog("Queries should fail because the shard can't initialize sharding state");
let error = assert.throws(function () {
    st.s.getDB("test").foo.find().itcount();
});

assert(
    ErrorCodes.ReplicaSetNotFound == error.code ||
        ErrorCodes.ExceededTimeLimit == error.code ||
        ErrorCodes.HostUnreachable == error.code ||
        ErrorCodes.FailedToSatisfyReadPreference == error.code,
);

jsTestLog("Restarting the config servers");
st.configRS.nodes.forEach((config) => {
    st.restartConfigServer(config);
});

print("Sleeping for 60 seconds to let the other shards restart their ReplicaSetMonitors");
sleep(60000);

jsTestLog("Queries against the original mongos should work again");
assert.eq(100, st.s.getDB("test").foo.find().itcount());

jsTestLog("Should now be possible to connect to the mongos that was started while the config " + "servers were down");
let newMongosConn = null;
let caughtException = null;
assert.soon(
    function () {
        try {
            newMongosConn = new Mongo(newMongosInfo.host);
            return true;
        } catch (e) {
            caughtException = e;
            return false;
        }
    },
    "Failed to connect to mongos after config servers were restarted: " + tojson(caughtException),
);

assert.eq(100, newMongosConn.getDB("test").foo.find().itcount());

st.stop({parallelSupported: false});
MongoRunner.stopMongos(newMongosInfo);
