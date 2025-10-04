/**
 * Test for making sure that the replica seed list in the config server does not
 * become invalid when a replica set reconfig happens.
 * @tags: [multiversion_incompatible]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig, reconnect} from "jstests/replsets/rslib.js";

// Skip the following checks since the removed node has wrong config and is still alive.
TestData.skipCheckDBHashes = true;
TestData.skipAwaitingReplicationOnShardsBeforeCheckingUUIDs = true;
TestData.skipCheckShardFilteringMetadata = true;

let NODE_COUNT = 3;
let st = new ShardingTest({
    shards: {rs0: {nodes: NODE_COUNT, oplogSize: 10}},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});
let replTest = st.rs0;
let mongos = st.s;

let shardDoc;
assert.soon(() => {
    shardDoc = mongos.getDB("config").shards.findOne();
    return NODE_COUNT == shardDoc.host.split(",").length; // seed list should contain all nodes
});

let confDoc = replTest.getReplSetConfigFromNode();
let secondary = replTest.getSecondary();

jsTestLog("Removing " + secondary.host + " from the set");

confDoc.members = confDoc.members.filter((member) => {
    return member.host !== secondary.host;
});
confDoc.version++;

reconfig(replTest, confDoc);

jsTest.log("Waiting for mongos to reflect change in shard replica set membership.");
let replView;
assert.soon(
    function () {
        let connPoolStats = mongos.getDB("admin").runCommand("connPoolStats");
        replView = connPoolStats.replicaSets[replTest.name].hosts;
        return replView.length == confDoc.members.length;
    },
    function () {
        return (
            "Expected to find " +
            confDoc.members.length +
            " nodes but found " +
            replView.length +
            " in " +
            tojson(replView)
        );
    },
);

jsTest.log("Waiting for config.shards to reflect change in shard replica set membership.");
assert.soon(
    function () {
        shardDoc = mongos.getDB("config").shards.findOne();
        // seed list should contain one less node
        return shardDoc.host.split(",").length == confDoc.members.length;
    },
    function () {
        return (
            "Expected to find " +
            confDoc.members.length +
            " nodes but found " +
            shardDoc.host.split(",").length +
            " in " +
            shardDoc.host
        );
    },
);

jsTestLog("Waiting for" + secondary.host + "replSetGetStatus to show that it was removed.");
assert.soonNoExcept(
    () => {
        // The secondary dropped connections when it was removed.
        reconnect(secondary);
        let status = secondary.getDB("admin").runCommand({replSetGetStatus: 1});
        jsTestLog(`replSetGetStatus: ${tojson(status)}`);
        return status.code === ErrorCodes.InvalidReplicaSetConfig;
    },
    "Waiting for" + secondary.host + "replSetGetStatus to show that it was removed",
    undefined /* timeout */,
    1000 /* intervalMS */,
);

st.stop({parallelSupported: false, skipValidation: true});
