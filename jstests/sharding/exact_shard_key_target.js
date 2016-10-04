//
// Verifies that shard key targeted update/delete operations go to exactly one shard when targeted
// by nested shard keys.
// SERVER-14138
//

var st = new ShardingTest({shards: 2, verbose: 4});

var mongos = st.s0;
var coll = mongos.getCollection("foo.bar");
var admin = mongos.getDB("admin");
var shards = mongos.getDB("config").shards.find().toArray();

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
printjson(admin.runCommand({movePrimary: coll.getDB().getName(), to: shards[0]._id}));
assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {"a.b": 1}}));
assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {"a.b": 0}}));
assert.commandWorked(
    admin.runCommand({moveChunk: coll.getFullName(), find: {"a.b": 0}, to: shards[1]._id}));

st.printShardingStatus();

//
// JustOne remove
coll.remove({});
assert.writeOK(coll.insert({_id: 1, a: {b: -1}}));
assert.writeOK(coll.insert({_id: 2, a: {b: 1}}));
// Need orphaned data to see the impact
assert.writeOK(st.shard0.getCollection(coll.toString()).insert({_id: 3, a: {b: 1}}));
assert.eq(1, coll.remove({a: {b: 1}}, {justOne: true}).nRemoved);
assert.eq(2,
          st.shard0.getCollection(coll.toString()).count() +
              st.shard1.getCollection(coll.toString()).count());

//
// Non-multi update
coll.remove({});
assert.writeOK(coll.insert({_id: 1, a: {b: 1}}));
assert.writeOK(coll.insert({_id: 2, a: {b: -1}}));
// Need orphaned data to see the impact
assert.writeOK(st.shard0.getCollection(coll.toString()).insert({_id: 3, a: {b: 1}}));
assert.eq(1, coll.update({a: {b: 1}}, {$set: {updated: true}}, {multi: false}).nMatched);
assert.eq(1,
          st.shard0.getCollection(coll.toString()).count({updated: true}) +
              st.shard1.getCollection(coll.toString()).count({updated: true}));

//
// Successive upserts (save()-style)
coll.remove({});
assert.writeOK(coll.update({_id: 1}, {_id: 1, a: {b: 1}}, {upsert: true}));
assert.writeOK(coll.update({_id: 1}, {_id: 1, a: {b: 1}}, {upsert: true}));
assert.eq(1,
          st.shard0.getCollection(coll.toString()).count() +
              st.shard1.getCollection(coll.toString()).count());

//
// Successive upserts (replacement-style)
coll.remove({});
assert.writeOK(coll.update({a: {b: 1}}, {a: {b: 1}}, {upsert: true}));
assert.writeOK(coll.update({a: {b: 1}}, {a: {b: 1}}, {upsert: true}));
assert.eq(1,
          st.shard0.getCollection(coll.toString()).count() +
              st.shard1.getCollection(coll.toString()).count());

//
// Successive upserts ($op-style)
coll.remove({});
assert.writeOK(coll.update({a: {b: 1}}, {$set: {upserted: true}}, {upsert: true}));
assert.writeOK(coll.update({a: {b: 1}}, {$set: {upserted: true}}, {upsert: true}));
assert.eq(1,
          st.shard0.getCollection(coll.toString()).count() +
              st.shard1.getCollection(coll.toString()).count());

jsTest.log("DONE!");
st.stop();
