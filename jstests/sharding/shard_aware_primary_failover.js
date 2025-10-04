/**
 * Test that a new primary that gets elected will properly perform shard initialization.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

// Setting CWWC for addShard to work, as implicitDefaultWC is set to w:1.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

let replTest = new ReplSetTest({nodes: 3});
replTest.startSet({shardsvr: ""});

let nodes = replTest.nodeList();
replTest.initiate(
    {
        _id: replTest.name,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1]},
            {_id: 2, host: nodes[2], arbiterOnly: true},
        ],
    },
    null,
    {initiateWithDefaultElectionTimeout: true},
);

let primaryConn = replTest.getPrimary();

let shardIdentityDoc = {
    _id: "shardIdentity",
    configsvrConnectionString: st.configRS.getURL(),
    shardName: "newShard",
    clusterId: ObjectId(),
};

// Simulate the upsert that is performed by a config server on addShard.
let shardIdentityQuery = {
    _id: shardIdentityDoc._id,
    shardName: shardIdentityDoc.shardName,
    clusterId: shardIdentityDoc.clusterId,
};
let shardIdentityUpdate = {
    $set: {configsvrConnectionString: shardIdentityDoc.configsvrConnectionString},
};
assert.commandWorked(
    primaryConn
        .getDB("admin")
        .system.version.update(shardIdentityQuery, shardIdentityUpdate, {upsert: true, writeConcern: {w: "majority"}}),
);

replTest.stopPrimary();
replTest.waitForPrimary(30000);

primaryConn = replTest.getPrimary();

let res = primaryConn.getDB("admin").runCommand({shardingState: 1});

assert(res.enabled);
assert.eq(shardIdentityDoc.shardName, res.shardName);
assert.eq(shardIdentityDoc.clusterId, res.clusterId);
assert.soon(
    () => shardIdentityDoc.configsvrConnectionString == primaryConn.adminCommand({shardingState: 1}).configServer,
);

replTest.stopSet();

st.stop();
