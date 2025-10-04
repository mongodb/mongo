// Tests that a mongos will correctly retry a stale shard version when read preference is used
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({
    shards: {rs0: {quiet: ""}, rs1: {quiet: ""}},
    mongos: 2,
    other: {mongosOptions: {verbose: 2}},
});

let testDB1 = st.s0.getDB("test");
let testDB2 = st.s1.getDB("test");

// Trigger a query on mongos 1 so it will have a view of test.user as being unsharded.
testDB1.user.findOne();

assert.commandWorked(testDB2.adminCommand({enableSharding: "test"}));
assert.commandWorked(testDB2.adminCommand({shardCollection: "test.user", key: {x: 1}}));
assert.commandWorked(testDB2.adminCommand({split: "test.user", middle: {x: 100}}));

let configDB2 = st.s1.getDB("config");
let chunkToMove = configDB2.chunks.find().sort({min: 1}).next();
let toShard = configDB2.shards.findOne({_id: {$ne: chunkToMove.shard}})._id;
assert.commandWorked(testDB2.adminCommand({moveChunk: "test.user", to: toShard, find: {x: 50}}));

// Insert a document into each chunk
assert.commandWorked(testDB2.user.insert({x: 30}));
assert.commandWorked(testDB2.user.insert({x: 130}));

// The testDB1 mongos does not know the chunk has been moved, and will retry
let cursor = testDB1.user.find({x: 30}).readPref("primary");
assert(cursor.hasNext());
assert.eq(30, cursor.next().x);

cursor = testDB1.user.find({x: 130}).readPref("primary");
assert(cursor.hasNext());
assert.eq(130, cursor.next().x);

st.stop();
