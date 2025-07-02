/**
 * Tests that the connection string in the config.shards document is correctly updated when a node
 * is added or removed from the replica set.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

const dbName = "testDb";
const collName = "testColl";
const testColl = st.s.getDB(dbName).getCollection(collName);

function getConfigShardsDoc(rst) {
    return st.s.getCollection("config.shards").findOne({
        _id: rst.isConfigServer ? "config" : rst.name
    });
}

const rs0Primary = st.rs0.getPrimary();
const rs0Config = st.rs0.getReplSetConfigFromNode();

const configShardsDoc0 = getConfigShardsDoc(st.rs0);
assert.eq(configShardsDoc0.host.split(",").length, 2);

let roleStr = st.rs0.isConfigServer ? 'configsvr' : 'shardsvr';
let replConfigForNewNode = {rsConfig: {votes: 0, priority: 0}};
replConfigForNewNode[roleStr] = '';

const newNode = st.rs0.add(replConfigForNewNode);
st.rs0.reInitiate();

assert.soon(() => {
    const configShardsDoc1 = getConfigShardsDoc(st.rs0);
    return configShardsDoc1.host.split(",").length === 3;
});

st.rs0.remove(newNode);
rs0Config.version++;
assert.commandWorked(rs0Primary.adminCommand({replSetReconfig: rs0Config, force: true}));

assert.soon(() => {
    const configShardsDoc2 = getConfigShardsDoc(st.rs0);
    return configShardsDoc2.host.split(",").length === 2;
});

assert.commandWorked(testColl.insert({x: 1}));

// Simulate restore procedure removing the replSetConfigVersion field.
// Shards should still be able to update the host names after this.
let shardsColl = st.s.getCollection("config.shards");
assert.commandWorked(shardsColl.update({_id: st.rs0.name}, {$unset: {replSetConfigVersion: ""}}));

st.rs0.add(replConfigForNewNode);
st.rs0.reInitiate();

assert.soon(() => {
    const configShardsDoc1 = getConfigShardsDoc(st.rs0);
    return configShardsDoc1.host.split(",").length === 3;
});

st.stop();
