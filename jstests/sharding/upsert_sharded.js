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

var upsertedResult = function(query, expr) {
    coll.remove({});
    return coll.update(query, expr, {upsert: true});
};

var upsertedField = function(query, expr, fieldName) {
    assert.writeOK(upsertedResult(query, expr));
    return coll.findOne()[fieldName];
};

var upsertedId = function(query, expr) {
    return upsertedField(query, expr, "_id");
};

var upsertedXVal = function(query, expr) {
    return upsertedField(query, expr, "x");
};

st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {x: 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}));

st.printShardingStatus();

// upserted update replacement would result in no shard key
assert.writeError(upsertedResult({x: 1}, {}));

// updates with upsert must contain shard key in query when $op style
assert.eq(1, upsertedXVal({x: 1}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal({x: {$eq: 1}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal({x: {$all: [1]}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal({x: {$in: [1]}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal({$and: [{x: {$eq: 1}}]}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal({$or: [{x: {$eq: 1}}]}, {$set: {a: 1}}));

// Missing shard key in query.
assert.commandFailedWithCode(upsertedResult({}, {$set: {a: 1, x: 1}}), ErrorCodes.ShardKeyNotFound);

// Missing equality match on shard key in query.
assert.commandFailedWithCode(upsertedResult({x: {$gt: 10}}, {$set: {a: 1, x: 5}}),
                             ErrorCodes.ShardKeyNotFound);

// Regex shard key value in query is ambigious and cannot be extracted for an equality match.
assert.commandFailedWithCode(upsertedResult({x: {$eq: /abc*/}}, {$set: {a: 1, x: "regexValue"}}),
                             ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(upsertedResult({x: {$eq: /abc/}}, {$set: {a: 1, x: /abc/}}),
                             ErrorCodes.ShardKeyNotFound);

// Shard key in query not extractable.
assert.commandFailedWithCode(upsertedResult({x: undefined}, {$set: {a: 1}}), ErrorCodes.BadValue);
assert.commandFailedWithCode(upsertedResult({x: [1, 2]}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(upsertedResult({x: {$eq: {$gt: 5}}}, {$set: {a: 1}}),
                             ErrorCodes.ShardKeyNotFound);

// nested field extraction always fails with non-nested key - like _id, we require setting the
// elements directly
assert.writeError(upsertedResult({"x.x": 1}, {$set: {a: 1}}));
assert.writeError(upsertedResult({"x.x": {$eq: 1}}, {$set: {a: 1}}));

coll.drop();

st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {'x.x': 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {'x.x': 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {'x.x': 0}, to: st.shard1.shardName, _waitForDelete: true}));

st.printShardingStatus();

// nested field extraction with nested shard key
assert.docEq({x: 1}, upsertedXVal({"x.x": 1}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal({"x.x": {$eq: 1}}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal({"x.x": {$all: [1]}}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal({$and: [{"x.x": {$eq: 1}}]}, {$set: {a: 1}}));
assert.docEq({x: 1}, upsertedXVal({$or: [{"x.x": {$eq: 1}}]}, {$set: {a: 1}}));

// Can specify siblings of nested shard keys
assert.docEq({x: 1, y: 1}, upsertedXVal({"x.x": 1, "x.y": 1}, {$set: {a: 1}}));
assert.docEq({x: 1, y: {z: 1}}, upsertedXVal({"x.x": 1, "x.y.z": 1}, {$set: {a: 1}}));

// No arrays at any level
assert.writeError(upsertedResult({"x.x": []}, {$set: {a: 1}}));
assert.writeError(upsertedResult({x: {x: []}}, {$set: {a: 1}}));
assert.writeError(upsertedResult({x: [{x: 1}]}, {$set: {a: 1}}));

// Can't set sub-fields of nested key
assert.writeError(upsertedResult({"x.x.x": {$eq: 1}}, {$set: {a: 1}}));

st.stop();
})();
