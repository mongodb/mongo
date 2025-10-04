// Test write re-routing on version mismatch.
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, mongos: 2});

let mongos = st.s;
let admin = mongos.getDB("admin");
let config = mongos.getDB("config");
let coll = st.s.getCollection("TestDB.coll");

assert.commandWorked(mongos.adminCommand({enableSharding: "TestDB", primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: "TestDB.coll", key: {_id: 1}}));

jsTest.log("Refreshing second mongos...");

let mongosB = st.s1;
let adminB = mongosB.getDB("admin");
let collB = mongosB.getCollection(coll + "");

// Make sure mongosB knows about the coll
assert.eq(0, collB.find().itcount());

jsTest.log("Moving chunk to create stale mongos...");
assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: st.shard1.shardName}));

jsTest.log("Inserting docs that needs to be retried...");

let nextId = -1;
for (var i = 0; i < 2; i++) {
    printjson("Inserting " + nextId);
    assert.commandWorked(collB.insert({_id: nextId--, hello: "world"}));
}

jsTest.log("Inserting doc which successfully goes through...");

// Do second write
assert.commandWorked(collB.insert({_id: nextId--, goodbye: "world"}));

// Assert that write went through
assert.eq(coll.find().itcount(), 3);

jsTest.log("Now try moving the actual chunk we're writing to...");

// Now move the actual chunk we're writing to
printjson(admin.runCommand({moveChunk: coll + "", find: {_id: -1}, to: st.shard1.shardName}));

jsTest.log("Inserting second docs to get written back...");

// Will fail entirely if too many of these, waiting for write to get applied can get too long.
for (var i = 0; i < 2; i++) {
    collB.insert({_id: nextId--, hello: "world"});
}

// Refresh server
printjson(adminB.runCommand({flushRouterConfig: 1}));

jsTest.log("Inserting second doc which successfully goes through...");

// Do second write
assert.commandWorked(collB.insert({_id: nextId--, goodbye: "world"}));

jsTest.log("All docs written this time!");

// Assert that writes went through.
assert.eq(coll.find().itcount(), 6);

st.stop();
