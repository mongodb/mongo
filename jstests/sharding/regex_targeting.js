//
// This checks to make sure that sharded regex queries behave the same as unsharded regex queries
//

var options = {
    mongosOptions: {binVersion: ""},
    shardOptions: {binVersion: ""},
    configOptions: {binVersion: ""}
};

var st = new ShardingTest({shards: 2, other: options});
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB("admin");
var shards = mongos.getDB("config").shards.find().toArray();

//
// Set up multiple collections to target with regex shard keys on two shards
//

var coll = mongos.getCollection("foo.bar");
var collSharded = mongos.getCollection("foo.barSharded");
var collCompound = mongos.getCollection("foo.barCompound");
var collNested = mongos.getCollection("foo.barNested");
var collHashed = mongos.getCollection("foo.barHashed");

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().toString()}));
st.ensurePrimaryShard(coll.getDB().toString(), shards[0]._id);

//
// Split the collection so that "abcde-0" and "abcde-1" go on different shards when possible
//

assert.commandWorked(admin.runCommand({shardCollection: collSharded.toString(), key: {a: 1}}));
assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {a: "abcde-1"}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: collSharded.toString(), find: {a: 0}, to: shards[1]._id, _waitForDelete: true}));

assert.commandWorked(
    admin.runCommand({shardCollection: collCompound.toString(), key: {a: 1, b: 1}}));
assert.commandWorked(
    admin.runCommand({split: collCompound.toString(), middle: {a: "abcde-1", b: 0}}));
assert.commandWorked(admin.runCommand({
    moveChunk: collCompound.toString(),
    find: {a: 0, b: 0},
    to: shards[1]._id,
    _waitForDelete: true
}));

assert.commandWorked(admin.runCommand({shardCollection: collNested.toString(), key: {'a.b': 1}}));
assert.commandWorked(admin.runCommand({split: collNested.toString(), middle: {'a.b': "abcde-1"}}));
assert.commandWorked(admin.runCommand({
    moveChunk: collNested.toString(),
    find: {a: {b: 0}},
    to: shards[1]._id,
    _waitForDelete: true
}));

assert.commandWorked(
    admin.runCommand({shardCollection: collHashed.toString(), key: {hash: "hashed"}}));

st.printShardingStatus();

//
//
// Cannot insert regex _id
assert.writeError(coll.insert({_id: /regex value/}));
assert.writeError(collSharded.insert({_id: /regex value/, a: 0}));
assert.writeError(collCompound.insert({_id: /regex value/, a: 0, b: 0}));
assert.writeError(collNested.insert({_id: /regex value/, a: {b: 0}}));
assert.writeError(collHashed.insert({_id: /regex value/, hash: 0}));

//
//
// (For now) we can insert a regex shard key
assert.writeOK(collSharded.insert({a: /regex value/}));
assert.writeOK(collCompound.insert({a: /regex value/, b: "other value"}));
assert.writeOK(collNested.insert({a: {b: /regex value/}}));
assert.writeOK(collHashed.insert({hash: /regex value/}));

//
//
// Query by regex should hit all matching keys, across all shards if applicable
coll.remove({});
assert.writeOK(coll.insert({a: "abcde-0"}));
assert.writeOK(coll.insert({a: "abcde-1"}));
assert.writeOK(coll.insert({a: /abcde.*/}));
assert.eq(coll.find().itcount(), coll.find({a: /abcde.*/}).itcount());

collSharded.remove({});
assert.writeOK(collSharded.insert({a: "abcde-0"}));
assert.writeOK(collSharded.insert({a: "abcde-1"}));
assert.writeOK(collSharded.insert({a: /abcde.*/}));
assert.eq(collSharded.find().itcount(), collSharded.find({a: /abcde.*/}).itcount());

collCompound.remove({});
assert.writeOK(collCompound.insert({a: "abcde-0", b: 0}));
assert.writeOK(collCompound.insert({a: "abcde-1", b: 0}));
assert.writeOK(collCompound.insert({a: /abcde.*/, b: 0}));
assert.eq(collCompound.find().itcount(), collCompound.find({a: /abcde.*/}).itcount());

collNested.remove({});
assert.writeOK(collNested.insert({a: {b: "abcde-0"}}));
assert.writeOK(collNested.insert({a: {b: "abcde-1"}}));
assert.writeOK(collNested.insert({a: {b: /abcde.*/}}));
assert.eq(collNested.find().itcount(), collNested.find({'a.b': /abcde.*/}).itcount());

collHashed.remove({});
while (st.shard0.getCollection(collHashed.toString()).count() == 0 ||
       st.shard1.getCollection(collHashed.toString()).count() == 0) {
    assert.writeOK(collHashed.insert({hash: "abcde-" + ObjectId().toString()}));
}
assert.writeOK(collHashed.insert({hash: /abcde.*/}));
assert.eq(collHashed.find().itcount(), collHashed.find({hash: /abcde.*/}).itcount());

//
//
// Update by regex should hit all matching keys, across all shards if applicable
coll.remove({});
assert.writeOK(coll.insert({a: "abcde-0"}));
assert.writeOK(coll.insert({a: "abcde-1"}));
assert.writeOK(coll.insert({a: /abcde.*/}));
assert.writeOK(coll.update({a: /abcde.*/}, {$set: {updated: true}}, {multi: true}));
assert.eq(coll.find().itcount(), coll.find({updated: true}).itcount());

collSharded.remove({});
assert.writeOK(collSharded.insert({a: "abcde-0"}));
assert.writeOK(collSharded.insert({a: "abcde-1"}));
assert.writeOK(collSharded.insert({a: /abcde.*/}));
assert.writeOK(collSharded.update({a: /abcde.*/}, {$set: {updated: true}}, {multi: true}));
assert.eq(collSharded.find().itcount(), collSharded.find({updated: true}).itcount());

collCompound.remove({});
assert.writeOK(collCompound.insert({a: "abcde-0", b: 0}));
assert.writeOK(collCompound.insert({a: "abcde-1", b: 0}));
assert.writeOK(collCompound.insert({a: /abcde.*/, b: 0}));
assert.writeOK(collCompound.update({a: /abcde.*/}, {$set: {updated: true}}, {multi: true}));
assert.eq(collCompound.find().itcount(), collCompound.find({updated: true}).itcount());

collNested.remove({});
assert.writeOK(collNested.insert({a: {b: "abcde-0"}}));
assert.writeOK(collNested.insert({a: {b: "abcde-1"}}));
assert.writeOK(collNested.insert({a: {b: /abcde.*/}}));
assert.writeOK(collNested.update({'a.b': /abcde.*/}, {$set: {updated: true}}, {multi: true}));
assert.eq(collNested.find().itcount(), collNested.find({updated: true}).itcount());

collHashed.remove({});
while (st.shard0.getCollection(collHashed.toString()).count() == 0 ||
       st.shard1.getCollection(collHashed.toString()).count() == 0) {
    assert.writeOK(collHashed.insert({hash: "abcde-" + ObjectId().toString()}));
}
assert.writeOK(collHashed.insert({hash: /abcde.*/}));
assert.writeOK(collHashed.update({hash: /abcde.*/}, {$set: {updated: true}}, {multi: true}));
assert.eq(collHashed.find().itcount(), collHashed.find({updated: true}).itcount());

//
//
// Upsert with op-style regex should fail on sharded collections
// Query clause is targeted, and regex in query clause is ambiguous
collSharded.remove({});
collCompound.remove({});
collNested.remove({});
assert.writeError(collSharded.update({a: /abcde.*/}, {$set: {a: /abcde.*/}}, {upsert: true}));
assert.writeError(
    collCompound.update({a: /abcde.*/}, {$set: {a: /abcde.*/, b: 1}}, {upsert: true}));
// Exact regex in query never equality
assert.writeError(
    collNested.update({'a.b': /abcde.*/}, {$set: {'a.b': /abcde.*/}}, {upsert: true}));
// Even nested regexes are not extracted in queries
assert.writeError(
    collNested.update({a: {b: /abcde.*/}}, {$set: {'a.b': /abcde.*/}}, {upsert: true}));
assert.writeError(collNested.update({c: 1}, {$set: {'a.b': /abcde.*/}}, {upsert: true}));

//
//
// Upsert by replacement-style regex should succeed on sharded collections
// Replacement clause is targeted, and regex is unambiguously a value
collSharded.remove({});
collCompound.remove({});
collNested.remove({});
assert.writeOK(collSharded.update({a: /abcde.*/}, {a: /abcde.*/}, {upsert: true}));
assert.writeOK(collCompound.update({a: /abcde.*/}, {a: /abcde.*/, b: 1}, {upsert: true}));
assert.writeOK(collNested.update({'a.b': /abcde.*/}, {a: {b: /abcde.*/}}, {upsert: true}));
assert.writeOK(collNested.update({a: {b: /abcde.*/}}, {a: {b: /abcde.*/}}, {upsert: true}));
assert.writeOK(collNested.update({c: 1}, {a: {b: /abcde.*/}}, {upsert: true}));

//
//
// Remove by regex should hit all matching keys, across all shards if applicable
coll.remove({});
assert.writeOK(coll.insert({a: "abcde-0"}));
assert.writeOK(coll.insert({a: "abcde-1"}));
assert.writeOK(coll.insert({a: /abcde.*/}));
assert.writeOK(coll.remove({a: /abcde.*/}));
assert.eq(0, coll.find({}).itcount());

collSharded.remove({});
assert.writeOK(collSharded.insert({a: "abcde-0"}));
assert.writeOK(collSharded.insert({a: "abcde-1"}));
assert.writeOK(collSharded.insert({a: /abcde.*/}));
assert.writeOK(collSharded.remove({a: /abcde.*/}));
assert.eq(0, collSharded.find({}).itcount());

collCompound.remove({});
assert.writeOK(collCompound.insert({a: "abcde-0", b: 0}));
assert.writeOK(collCompound.insert({a: "abcde-1", b: 0}));
assert.writeOK(collCompound.insert({a: /abcde.*/, b: 0}));
assert.writeOK(collCompound.remove({a: /abcde.*/}));
assert.eq(0, collCompound.find({}).itcount());

collNested.remove({});
assert.writeOK(collNested.insert({a: {b: "abcde-0"}}));
assert.writeOK(collNested.insert({a: {b: "abcde-1"}}));
assert.writeOK(collNested.insert({a: {b: /abcde.*/}}));
assert.writeOK(collNested.remove({'a.b': /abcde.*/}));
assert.eq(0, collNested.find({}).itcount());

collHashed.remove({});
while (st.shard0.getCollection(collHashed.toString()).count() == 0 ||
       st.shard1.getCollection(collHashed.toString()).count() == 0) {
    assert.writeOK(collHashed.insert({hash: "abcde-" + ObjectId().toString()}));
}
assert.writeOK(collHashed.insert({hash: /abcde.*/}));
assert.writeOK(collHashed.remove({hash: /abcde.*/}));
assert.eq(0, collHashed.find({}).itcount());

//
//
// Query/Update/Remove by nested regex is different depending on how the nested regex is specified
coll.remove({});
assert.writeOK(coll.insert({a: {b: "abcde-0"}}));
assert.writeOK(coll.insert({a: {b: "abcde-1"}}));
assert.writeOK(coll.insert({a: {b: /abcde.*/}}));
assert.eq(1, coll.find({a: {b: /abcde.*/}}).itcount());
assert.writeOK(coll.update({a: {b: /abcde.*/}}, {$set: {updated: true}}, {multi: true}));
assert.eq(1, coll.find({updated: true}).itcount());
assert.writeOK(coll.remove({a: {b: /abcde.*/}}));
assert.eq(2, coll.find().itcount());

collNested.remove({});
assert.writeOK(collNested.insert({a: {b: "abcde-0"}}));
assert.writeOK(collNested.insert({a: {b: "abcde-1"}}));
assert.writeOK(collNested.insert({a: {b: /abcde.*/}}));
assert.eq(1, collNested.find({a: {b: /abcde.*/}}).itcount());
assert.writeOK(collNested.update({a: {b: /abcde.*/}}, {$set: {updated: true}}, {multi: true}));
assert.eq(1, collNested.find({updated: true}).itcount());
assert.writeOK(collNested.remove({a: {b: /abcde.*/}}));
assert.eq(2, collNested.find().itcount());

jsTest.log("DONE!");
st.stop();
