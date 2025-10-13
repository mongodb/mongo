// Tests that mongos and shard mongods can both be started up successfully when there is no config
// server, and that they will wait until there is a config server online before handling any
// sharding operations.
//
// However, if a shard mongod is restarted without --shardsvr mode then the shard mongod
// will NOT wait until there is a config server online before continuing replication.
//
// This test involves restarting a standalone shard, so cannot be run on ephemeral storage engines.
// A restarted standalone will lose all data when using an ephemeral storage engine.
// @tags: [requires_persistence]

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {stopServerReplication} from "jstests/libs/write_concern_util.js";

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
    rs1: {nodes: [{}, {rsConfig: {votes: 0, priority: 0}}]},
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

// We pause replication on one of the replica set shard secondaries and perform some additional
// writes to later verify the secondary will catch up when --shardsvr is omitted.
const secondary = st.rs1.getSecondary();
stopServerReplication(secondary);

const collection = st.s0.getCollection("test.foo");
for (let i = 75; i < 100; ++i) {
    assert.commandWorked(collection.update({_id: i}, {$inc: {x: 1}}, {writeConcern: {w: "majority"}}));
}

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

jsTestLog("Restarting a shard WITHOUT --shardsvr while there are no config servers up");
// We won't be able to compare dbHashes because server replication was stopped on the secondary.
st.rs1.stopSet(undefined, true, {skipCheckDBHashes: true, skipValidation: true});

for (let node of st.rs1.nodes) {
    delete node.fullOptions.shardsvr;

    st.rs1.start(node.nodeId, undefined, /*restart=*/ true, /*waitForHealth=*/ false);
    if (node.nodeId === 0) {
        // We prevent the node from being or becoming primary because we want to exercise the case
        // where a secondary mongod is syncing from another secondary with the config server replica
        // set being entirely unavailable. This is also why we must restart the shard mongod
        // processes individually.
        st.rs1.freeze(node);
    }
}

// Verify the non-voting shard mongod successfully syncs from the former primary.
st.rs1.awaitReplication(undefined, undefined, undefined, undefined, st.rs1.nodes[0]);

jsTestLog("Restarting a shard WITH --shardsvr while there are no config servers up");
// Since there is intentionally no primary for the replica set shard, we won't be able to compare
// dbHashes still.
st.rs1.stopSet(undefined, true, {skipCheckDBHashes: true, skipValidation: true});

st.rs1.nodes.forEach((node) => (node.fullOptions.shardsvr = ""));
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
