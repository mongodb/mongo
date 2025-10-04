/**
 * Tests for shard aware initialization on secondaries during startup and shard
 * identity document creation.
 * @tags: [requires_persistence]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

let replTest = new ReplSetTest({nodes: 2});
replTest.startSet({shardsvr: ""});
let nodeList = replTest.nodeList();
replTest.initiate({
    _id: replTest.name,
    members: [
        {_id: 0, host: nodeList[0], priority: 1},
        {_id: 1, host: nodeList[1], priority: 0},
    ],
});

let priConn = replTest.getPrimary();

let configConnStr = st.configRS.getURL();

let shardIdentityDoc = {
    _id: "shardIdentity",
    configsvrConnectionString: configConnStr,
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
    priConn
        .getDB("admin")
        .system.version.update(shardIdentityQuery, shardIdentityUpdate, {upsert: true, writeConcern: {w: 2}}),
);

let secConn = replTest.getSecondary();
secConn.setSecondaryOk();

let res = secConn.getDB("admin").runCommand({shardingState: 1});

assert(res.enabled, tojson(res));
assert.eq(shardIdentityDoc.shardName, res.shardName);
assert.eq(shardIdentityDoc.clusterId, res.clusterId);
assert.soon(() => shardIdentityDoc.configsvrConnectionString == secConn.adminCommand({shardingState: 1}).configServer);

replTest.restart(replTest.getNodeId(secConn));
replTest.waitForPrimary();
replTest.awaitSecondaryNodes();

secConn = replTest.getSecondary();
secConn.setSecondaryOk();

res = secConn.getDB("admin").runCommand({shardingState: 1});

assert(res.enabled, tojson(res));
assert.eq(shardIdentityDoc.shardName, res.shardName);
assert.eq(shardIdentityDoc.clusterId, res.clusterId);
assert.soon(() => shardIdentityDoc.configsvrConnectionString == secConn.adminCommand({shardingState: 1}).configServer);

replTest.stopSet();

st.stop();
