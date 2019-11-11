//
// Upsert behavior tests for sharding
// NOTE: Generic upsert behavior tests belong in the core suite
//
(function() {
'use strict';

var st = new ShardingTest({shards: 2, mongos: 1});

var mongos = st.s0;
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("foo.bar");

assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
st.ensurePrimaryShard(coll.getDB().getName(), st.shard1.shardName);

var upsertedResult = function(upsertColl, query, expr) {
    coll.remove({});
    return upsertColl.update(query, expr, {upsert: true});
};

var upsertedField = function(upsertColl, query, expr, fieldName) {
    assert.commandWorked(upsertedResult(upsertColl, query, expr));
    return upsertColl.findOne()[fieldName];
};

var upsertedXVal = function(upsertColl, query, expr) {
    return upsertedField(upsertColl, query, expr, "x");
};

st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {x: 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}));

st.printShardingStatus();

// Upserted replacement update can result in no shard key.
assert.commandWorked(upsertedResult(coll, {x: -1}, {_id: 1}));
assert.docEq(coll.findOne({}), {_id: 1});

// Upserted op style update will propagate shard key by default.
assert.commandWorked(upsertedResult(coll, {x: -1}, {$set: {_id: 1}}));
assert.docEq(coll.findOne({}), {_id: 1, x: -1});

// Upserted op style update can unset propagated shard key.
assert.commandWorked(upsertedResult(coll, {x: -1}, {$set: {_id: 1}, $unset: {x: 1}}));
assert.docEq(coll.findOne({}), {_id: 1});

// updates with upsert must contain shard key in query when $op style
assert.eq(1, upsertedXVal(coll, {x: 1}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {x: {$eq: 1}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {x: {$all: [1]}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {x: {$in: [1]}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {$and: [{x: {$eq: 1}}]}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {$or: [{x: {$eq: 1}}]}, {$set: {a: 1}}));

// Missing shard key in query.
assert.commandFailedWithCode(upsertedResult(coll, {}, {$set: {a: 1, x: 1}}),
                             ErrorCodes.ShardKeyNotFound);

// Missing equality match on shard key in query.
assert.commandFailedWithCode(upsertedResult(coll, {x: {$gt: 10}}, {$set: {a: 1, x: 5}}),
                             ErrorCodes.ShardKeyNotFound);

// Regex shard key value in query is ambigious and cannot be extracted for an equality match.
assert.commandFailedWithCode(
    upsertedResult(coll, {x: {$eq: /abc*/}}, {$set: {a: 1, x: "regexValue"}}),
    ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(upsertedResult(coll, {x: {$eq: /abc/}}, {$set: {a: 1, x: /abc/}}),
                             ErrorCodes.ShardKeyNotFound);

// Shard key in query not extractable.
assert.commandFailedWithCode(upsertedResult(coll, {x: undefined}, {$set: {a: 1}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(upsertedResult(coll, {x: [1, 2]}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(upsertedResult(coll, {x: {$eq: {$gt: 5}}}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);

// nested field extraction always fails with non-nested key - like _id, we require setting the
// elements directly
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": 1}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": {$eq: 1}}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);

coll.drop();

st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {'x.x': 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {'x.x': 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {'x.x': 0}, to: st.shard1.shardName, _waitForDelete: true}));

st.printShardingStatus();

// Upserted replacement update can result in no shard key with nested shard key.
assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {_id: 1}));
assert.docEq(coll.findOne({}), {_id: 1});

// Upserted op style update will propagate shard key by default with nested shard key.
assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {$set: {_id: 1}}));
assert.docEq(coll.findOne({}), {_id: 1, x: {x: -1}});

// Upserted op style update can unset propagated shard key fields with nested shard key.
assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {$set: {_id: 1}, $unset: {"x.x": 1}}));
assert.docEq(coll.findOne({}), {_id: 1, x: {}});

assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {$set: {_id: 1}, $unset: {"x": 1}}));
assert.docEq(coll.findOne({}), {_id: 1});

// nested field extraction with nested shard key
assert.docEq({x: 1}, upsertedXVal(coll, {"x.x": 1}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal(coll, {"x.x": {$eq: 1}}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal(coll, {"x.x": {$all: [1]}}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal(coll, {$and: [{"x.x": {$eq: 1}}]}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal(coll, {$or: [{"x.x": {$eq: 1}}]}, {$set: {a: 1}}));

// Can specify siblings of nested shard keys
assert.docEq({x: 1, y: 1}, upsertedXVal(coll, {"x.x": 1, "x.y": 1}, {$set: {a: 1}}));
assert.docEq({x: 1, y: {z: 1}}, upsertedXVal(coll, {"x.x": 1, "x.y.z": 1}, {$set: {a: 1}}));

// No arrays at any level for targeting.
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": []}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(upsertedResult(coll, {x: {x: []}}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(upsertedResult(coll, {x: [{x: 1}]}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);

// No arrays at any level for document insertion for replacement and op style updates.
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {$set: {x: {x: []}}}),
                             ErrorCodes.NotSingleValueField);
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {$set: {x: [{x: 1}]}}),
                             ErrorCodes.NotSingleValueField);

assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {x: {x: []}}),
                             ErrorCodes.NotSingleValueField);
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {x: [{x: 1}]}),
                             ErrorCodes.NotSingleValueField);

// Can't set sub-fields of nested key
assert.commandFailedWithCode(upsertedResult(coll, {"x.x.x": {$eq: 1}}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);

st.stop();
})();
