import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({
    shards: 1,
});

let shardPrimary = st.rs0.getPrimary();
let shardPrimaryConfigDB = shardPrimary.getDB("config");

let testDB = st.s.getDB("test1");

// Create a sharded collection with primary shard 0.
assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: testDB.foo.getFullName(), key: {a: 1}}));
assert.commandWorked(st.s.adminCommand({split: testDB.foo.getFullName(), middle: {a: 0}}));
assert.commandWorked(st.s.adminCommand({split: testDB.foo.getFullName(), middle: {a: -1000}}));
assert.commandWorked(st.s.adminCommand({split: testDB.foo.getFullName(), middle: {a: +1000}}));

assert.writeOK(st.s.getDB("test1").foo.insert({_id: "id1", a: 1}));
assert.neq(null, st.s.getDB("test1").foo.findOne({_id: "id1", a: 1}));

assert.writeOK(st.s.getDB("test1").foo.insert({_id: "id2", a: -1}));
assert.neq(null, st.s.getDB("test1").foo.findOne({_id: "id2", a: -1}));

const collection = st.s.getDB("config").getCollection("collections").findOne({_id: "test1.foo"});

// Manually clear the 'historyIsAt40' field from the config server and the history entries from
// the shards' cache collections in order to simulate a wrong upgrade due to SERVER-62065
assert.writeOK(
    st.s.getDB("config").chunks.update({uuid: collection.uuid}, {"$unset": {historyIsAt40: ""}}, {multi: true}),
);
assert.writeOK(
    shardPrimaryConfigDB.getCollection("cache.chunks.test1.foo").update({}, {"$unset": {history: ""}}, {multi: true}),
);

assert.commandWorked(st.s.adminCommand({repairShardedCollectionChunksHistory: "test1.foo"}));

// Make sure chunks for test1.foo were given history after repair
let chunks = st.s.getDB("config").getCollection("chunks").find({uuid: collection.uuid}).toArray();
assert.eq(chunks.length, 4);
chunks.forEach((chunk) => {
    assert.neq(null, chunk);
    assert(chunk.hasOwnProperty("history"), "test1.foo does not have a history after repair");
    assert(chunk.hasOwnProperty("historyIsAt40"), "test1.foo does not have a historyIsAt40 after repair");
});
chunks = shardPrimaryConfigDB.getCollection("cache.chunks.test1.foo").find().toArray();
assert.eq(chunks.length, 4);
chunks.forEach((chunk) => {
    assert.neq(null, chunk);
    assert(chunk.hasOwnProperty("history"), "test1.foo does not have a history on the shard after repair");
});

st.stop();
