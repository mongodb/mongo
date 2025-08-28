/**
 * Tests whether new sharding is detected on insert by mongos
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 10, mongos: 3});

let mongosA = st.s0;
let mongosB = st.s1;
let mongosC = st.s2;

let admin = mongosA.getDB("admin");
let config = mongosA.getDB("config");

let collA = mongosA.getCollection("foo.bar");
let collB = mongosB.getCollection("" + collA);
let collC = mongosB.getCollection("" + collA);

let shards = [
    st.shard0,
    st.shard1,
    st.shard2,
    st.shard3,
    st.shard4,
    st.shard5,
    st.shard6,
    st.shard7,
    st.shard8,
    st.shard9,
];

assert.commandWorked(admin.runCommand({enableSharding: "" + collA.getDB(), primaryShard: st.shard1.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: "" + collA, key: {_id: 1}}));

jsTestLog("Splitting up the collection...");

// Split up the collection
for (var i = 0; i < shards.length; i++) {
    assert.commandWorked(admin.runCommand({split: "" + collA, middle: {_id: i}}));
    assert.commandWorked(admin.runCommand({moveChunk: "" + collA, find: {_id: i}, to: shards[i].shardName}));
}

mongosB.getDB("admin").runCommand({flushRouterConfig: 1});
mongosC.getDB("admin").runCommand({flushRouterConfig: 1});

printjson(collB.count());
printjson(collC.count());

// Change up all the versions...
for (var i = 0; i < shards.length; i++) {
    assert.commandWorked(
        admin.runCommand({moveChunk: "" + collA, find: {_id: i}, to: shards[(i + 1) % shards.length].shardName}),
    );
}

// Make sure mongos A is up-to-date
mongosA.getDB("admin").runCommand({flushRouterConfig: 1});

jsTestLog("Running count!");

printjson(collB.count());
printjson(collC.find().toArray());

st.stop();
