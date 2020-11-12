// Ensure you can't shard on an array key
// @tags: [
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
'use strict';

var st = new ShardingTest({shards: 3});

var mongos = st.s0;

const kDbName = 'TestDB';
const kCollName = 'Foo';
const kFullNs = kDbName + '.' + kCollName;

const session = st.s.startSession({retryWrites: true});
const sessionDB = session.getDatabase(kDbName);
let coll = mongos.getCollection(kFullNs);

st.shardColl(coll, {_id: 1, i: 1}, {_id: ObjectId(), i: 1});

printjson(mongos.getDB("config").chunks.find().toArray());

print("1: insert some invalid data");

var value = null;

// Insert an object with invalid array key
assert.commandFailedWithCode(coll.insert({i: [1, 2]}), ErrorCodes.ShardKeyNotFound);

// Insert an object with all the right fields, but an invalid array val for _id
assert.commandFailedWithCode(coll.insert({_id: [1, 2], i: 3}), ErrorCodes.ShardKeyNotFound);

// Insert an object with valid array key
assert.commandWorked(coll.insert({i: 1}));

// Update the value with valid other field
value = coll.findOne({i: 1});
assert.commandWorked(coll.update(value, {$set: {j: 2}}));

// Update the value with invalid other fields
value = coll.findOne({i: 1});
assert.commandFailedWithCode(sessionDB[kCollName].update(value, Object.merge(value, {i: [3]})),
                             ErrorCodes.NotSingleValueField);

// Multi-update the value with invalid other fields
value = coll.findOne({i: 1});
assert.commandFailedWithCode(coll.update(value, Object.merge(value, {i: [3, 4]}), false, true),
                             ErrorCodes.InvalidOptions);

// Multi-update the value with other fields (won't work, but no error)
value = coll.findOne({i: 1});
assert.commandWorked(coll.update(Object.merge(value, {i: [1, 1]}), {$set: {k: 4}}, false, true));
assert.docEq(coll.findOne({i: 1}, {_id: 0}), {i: 1, j: 2});

// Query the value with other fields (won't work, but no error)
value = coll.findOne({i: 1});
coll.find(Object.merge(value, {i: [1, 1]})).toArray();

// Can't remove using multikey, but shouldn't error
value = coll.findOne({i: 1});
coll.remove(Object.extend(value, {i: [1, 2, 3, 4]}));

// Can't remove using multikey, but shouldn't error
value = coll.findOne({i: 1});
assert.commandWorked(coll.remove(Object.extend(value, {i: [1, 2, 3, 4, 5]})));
assert.eq(coll.find().itcount(), 1);

value = coll.findOne({i: 1});
assert.commandWorked(coll.remove(Object.extend(value, {i: 1})));
assert.eq(coll.find().itcount(), 0);

coll.ensureIndex({_id: 1, i: 1, j: 1});
// Can insert document that will make index into a multi-key as long as it's not part of shard
// key.
coll.remove({});
assert.commandWorked(coll.insert({i: 1, j: [1, 2]}));
assert.eq(coll.find().itcount(), 1);

// Same is true for updates.
coll.remove({});
coll.insert({_id: 1, i: 1});
assert.commandWorked(coll.update({_id: 1, i: 1}, {_id: 1, i: 1, j: [1, 2]}));
assert.eq(coll.find().itcount(), 1);

// Same for upserts.
coll.remove({});
assert.commandWorked(coll.update({_id: 1, i: 1}, {_id: 1, i: 1, j: [1, 2]}, true));
assert.eq(coll.find().itcount(), 1);

jsTestLog("Testing dotted path shard key with array value inserts.");
const kDottedCollName = 'collDotted';
const kDottedFullNs = kDbName + '.' + kDottedCollName;
const dottedColl = mongos.getCollection(kDottedFullNs);

assert.commandWorked(mongos.adminCommand({shardCollection: kDottedFullNs, key: {"a.b": 1}}));

assert.commandFailedWithCode(dottedColl.insert({a: [{b: -5}, {b: 5}]}),
                             ErrorCodes.ShardKeyNotFound);
assert.eq(null, dottedColl.findOne({"a.b": 5}));
assert.eq(null, dottedColl.findOne({"a.b": -5}));

jsTestLog("Testing dotted path shard key with array value updates.");

assert.commandWorked(dottedColl.insert({a: {b: 1}}));

assert.commandFailedWithCode(dottedColl.update({a: {b: 1}}, {a: [{b: -5}, {b: 5}]}),
                             ErrorCodes.NotSingleValueField);

assert.commandWorked(sessionDB[kDottedCollName].update({a: {b: 1}}, {}));

printjson("Sharding-then-inserting-multikey tested, now trying inserting-then-sharding-multikey");

// Insert a bunch of data then shard over key which is an array
coll = mongos.getCollection("" + coll + "2");
for (var i = 0; i < 10; i++) {
    // TODO : does not check weird cases like [ i, i ]
    assert.commandWorked(coll.insert({i: [i, i + 1]}));
}

coll.ensureIndex({_id: 1, i: 1});

try {
    st.shardColl(coll, {_id: 1, i: 1}, {_id: ObjectId(), i: 1});
} catch (e) {
    print("Correctly threw error on sharding with multikey index.");
}

st.printShardingStatus();

// Insert a bunch of data then shard over key which is not an array
coll = mongos.getCollection("" + coll + "3");
for (var i = 0; i < 10; i++) {
    // TODO : does not check weird cases like [ i, i ]
    assert.commandWorked(coll.insert({i: i}));
}

coll.ensureIndex({_id: 1, i: 1});

st.shardColl(coll, {_id: 1, i: 1}, {_id: ObjectId(), i: 1});

st.printShardingStatus();

st.stop();
})();
