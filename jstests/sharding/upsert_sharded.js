//
// Upsert behavior tests for sharding. NOTE: Generic
// upsert behavior tests belong in the core suite
//
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 1});

const mongos = st.s0;
const admin = mongos.getDB("admin");
const kDbName = "test";

assert(admin.runCommand({enableSharding: kDbName + "", primaryShard: st.shard0.name}).ok);

const coll = mongos.getCollection(kDbName + ".bar");

const upsertSuppliedResult = function(upsertColl, query, newDoc) {
    assert.commandWorked(upsertColl.remove({}));
    return coll.runCommand({
        update: coll.getName(),
        updates: [{
            q: query,
            u: [{$addFields: {unused: true}}],
            c: {new: newDoc},
            upsert: true,
            upsertSupplied: true
        }]
    });
};

const upsertedResult = function(upsertColl, query, expr) {
    assert.commandWorked(upsertColl.remove({}));
    return upsertColl.update(query, expr, {upsert: true});
};

const upsertedField = function(upsertColl, query, expr, fieldName) {
    assert.commandWorked(upsertedResult(upsertColl, query, expr));
    return upsertColl.findOne()[fieldName];
};

const upsertedXVal = function(upsertColl, query, expr) {
    return upsertedField(upsertColl, query, expr, "x");
};

//
// Tests for non-nested shard key.
//
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {x: 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}));

st.printShardingStatus();

// Upserted replacement update can result in no shard key.
assert.commandWorked(upsertedResult(coll, {x: -1}, {_id: 1}));
assert.docEq({_id: 1}, coll.findOne({}));

// Upserted with supplied document can result in no shard key.
assert.commandWorked(upsertSuppliedResult(coll, {x: -1}, {_id: 1}));
assert.docEq({_id: 1}, coll.findOne({}));

// Upserted op style update will propagate shard key by default.
assert.commandWorked(upsertedResult(coll, {x: -1}, {$set: {_id: 1}}));
assert.docEq({_id: 1, x: -1}, coll.findOne({}));

// Upserted op style update can unset propagated shard key.
assert.commandWorked(upsertedResult(coll, {x: -1}, {$set: {_id: 1}, $unset: {x: 1}}));
assert.docEq({_id: 1}, coll.findOne({}));

// Updates with upsert must contain shard key in query when $op style
assert.eq(1, upsertedXVal(coll, {x: 1}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {x: {$eq: 1}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {x: {$all: [1]}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {x: {$in: [1]}}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {$and: [{x: {$eq: 1}}]}, {$set: {a: 1}}));
assert.eq(1, upsertedXVal(coll, {$or: [{x: {$eq: 1}}]}, {$set: {a: 1}}));

assert.eq(1, upsertedXVal(coll, {}, {$set: {a: 1, x: 1}}));
assert.eq(5, upsertedXVal(coll, {x: {$gt: 10}}, {$set: {a: 1, x: 5}}));
assert.eq("regexValue", upsertedXVal(coll, {x: {$eq: /abc*/}}, {$set: {a: 1, x: "regexValue"}}));
assert.eq(/abc/, upsertedXVal(coll, {x: {$eq: /abc/}}, {$set: {a: 1, x: /abc/}}));
assert.eq({$gt: 5}, upsertedXVal(coll, {x: {$eq: {$gt: 5}}}, {$set: {a: 1}}));
assert.eq({x: 1}, upsertedXVal(coll, {"x.x": 1}, {$set: {a: 1}}));
assert.eq({x: 1}, upsertedXVal(coll, {"x.x": {$eq: 1}}, {$set: {a: 1}}));

// Shard key in query not extractable.
assert.commandFailedWithCode(upsertedResult(coll, {x: undefined}, {$set: {a: 1}}),
                             ErrorCodes.BadValue);
// With the introduction of PM-1632, we can execute upsert not targeted to a single shard, but since
// shard key fields cannot contain arrays or array descendants, we will still throw
// NotSingleValueField when we go to upsert the document.
assert.commandFailedWithCode(upsertedResult(coll, {x: [1, 2]}, {$set: {a: 1}}),
                             [ErrorCodes.ShardKeyNotFound, ErrorCodes.NotSingleValueField]);

coll.drop();

//
// Tests for nested shard key.
//
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {'x.x': 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {'x.x': 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {'x.x': 0}, to: st.shard1.shardName, _waitForDelete: true}));

st.printShardingStatus();

// Upserted replacement update can result in no shard key with nested shard key.
assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {_id: 1}));
assert.docEq({_id: 1}, coll.findOne({}));

// Upserted with supplied document can result in no shard key with nested shard key.
assert.commandWorked(upsertSuppliedResult(coll, {"x.x": -1}, {_id: 1}));
assert.docEq({_id: 1}, coll.findOne({}));

// Upserted op style update will propagate shard key by default with nested shard key.
assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {$set: {_id: 1}}));
assert.docEq({_id: 1, x: {x: -1}}, coll.findOne({}));

// Upserted op style update can unset propagated shard key fields with nested shard key.
assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {$set: {_id: 1}, $unset: {"x.x": 1}}));
assert.docEq({_id: 1, x: {}}, coll.findOne({}));

assert.commandWorked(upsertedResult(coll, {"x.x": -1}, {$set: {_id: 1}, $unset: {"x": 1}}));
assert.docEq({_id: 1}, coll.findOne({}));

// Nested field extraction with nested shard key
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

// No arrays at any level for document insertion for replacement, supplied, and op updates.
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {$set: {x: {x: []}}}),
                             ErrorCodes.NotSingleValueField);
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {$set: {x: [{x: 1}]}}),
                             ErrorCodes.NotSingleValueField);

assert.commandFailedWithCode(upsertSuppliedResult(coll, {"x.x": -1}, {x: {x: []}}),
                             ErrorCodes.NotSingleValueField);
assert.commandFailedWithCode(upsertSuppliedResult(coll, {"x.x": -1}, {x: [{x: 1}]}),
                             ErrorCodes.NotSingleValueField);

assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {x: {x: []}}),
                             ErrorCodes.NotSingleValueField);
assert.commandFailedWithCode(upsertedResult(coll, {"x.x": -1}, {x: [{x: 1}]}),
                             ErrorCodes.NotSingleValueField);

assert.eq({x: {x: 1}}, upsertedXVal(coll, {"x.x.x": {$eq: 1}}, {$set: {a: 1}}));

assert.eq({x: {x: 1}}, upsertedXVal(coll, {"x.x.x": {$eq: 1}}, {$set: {a: 1}}));

coll.drop();

//
// Tests for nested _id shard key.
//
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {'_id.x': 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {'_id.x': 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {'_id.x': 0}, to: st.shard1.shardName, _waitForDelete: true}));

st.printShardingStatus();

// No upsert type can result in a missing shard key for nested _id key.
assert.commandWorked(upsertedResult(coll, {_id: {x: -1}}, {}));
assert.docEq({_id: {x: -1}}, coll.findOne({}));

assert.commandWorked(upsertSuppliedResult(coll, {_id: {x: -1}}, {}));
assert.docEq({_id: {x: -1}}, coll.findOne({}));

assert.commandWorked(upsertedResult(coll, {_id: {x: -1}}, {$set: {y: 1}}));
assert.docEq({_id: {x: -1}, y: 1}, coll.findOne({}));

assert.commandFailedWithCode(
    upsertedResult(coll, {_id: {x: -1}}, {$set: {y: 1}, $unset: {"_id.x": 1}}),
    ErrorCodes.ImmutableField);

// All update types can re-state shard key for nested _id key.
assert.commandWorked(upsertedResult(coll, {_id: {x: -1}}, {_id: {x: -1}, y: 1}));
assert.docEq({_id: {x: -1}, y: 1}, coll.findOne({}));

assert.commandWorked(upsertSuppliedResult(coll, {_id: {x: -1}}, {_id: {x: -1}, y: 1}));
assert.docEq({_id: {x: -1}, y: 1}, coll.findOne({}));

assert.commandWorked(upsertedResult(coll, {_id: {x: -1}}, {$set: {_id: {x: -1}, y: 1}}));
assert.docEq({_id: {x: -1}, y: 1}, coll.findOne({}));

assert.commandWorked(upsertedResult(coll, {_id: {x: -1}}, {$set: {"_id.x": -1, y: 1}}));
assert.docEq({_id: {x: -1}, y: 1}, coll.findOne({}));

// No upsert type can modify shard key for nested _id key.
assert.commandFailedWithCode(upsertedResult(coll, {_id: {x: -1}}, {_id: {x: -2}}),
                             ErrorCodes.ImmutableField);

assert.commandFailedWithCode(upsertSuppliedResult(coll, {_id: {x: -1}}, {_id: {x: -2}}),
                             ErrorCodes.ImmutableField);

assert.commandFailedWithCode(upsertedResult(coll, {_id: {x: -1}}, {$set: {_id: {x: -2}}}),
                             ErrorCodes.ImmutableField);

// No upsert type can add new _id subfield for nested _id key.
assert.commandFailedWithCode(upsertedResult(coll, {_id: {x: -1}}, {_id: {x: -1, y: -1}}),
                             ErrorCodes.ImmutableField);

assert.commandFailedWithCode(upsertSuppliedResult(coll, {_id: {x: -1}}, {_id: {x: -1, y: -1}}),
                             ErrorCodes.ImmutableField);

assert.commandFailedWithCode(
    upsertedResult(coll, {_id: {x: -1}}, {$set: {"_id.x": -1, "_id.y": -1}}),
    ErrorCodes.ImmutableField);

// No upsert type can remove non-shardkey _id subfield for nested _id key.
assert.commandFailedWithCode(upsertedResult(coll, {_id: {x: -1, y: -1}}, {_id: {x: -1}}),
                             ErrorCodes.ImmutableField);

assert.commandFailedWithCode(upsertSuppliedResult(coll, {_id: {x: -1, y: -1}}, {_id: {x: -1}}),
                             ErrorCodes.ImmutableField);

assert.commandFailedWithCode(upsertedResult(coll, {_id: {x: -1, y: -1}}, {$unset: {"_id.y": 1}}),
                             ErrorCodes.ImmutableField);

// Incorrect format of _id or shard key errors.
assert.commandFailedWithCode(
    upsertedResult(coll, {_id: {x: [1]}}, {}),
    ErrorCodes.ShardKeyNotFound);  // Shard key cannot contain array values or array descendants.
assert.commandFailedWithCode(upsertedResult(coll, {"_id.x": [1]}, {}),
                             ErrorCodes.NotExactValueField);
assert.commandFailedWithCode(upsertedResult(coll, {_id: [{x: 1}]}, {}),
                             ErrorCodes.NotSingleValueField);

assert.commandFailedWithCode(
    upsertSuppliedResult(coll, {_id: {x: [1]}}, {}),
    ErrorCodes.ShardKeyNotFound);  // Shard key cannot contain array values or array descendants.
assert.commandFailedWithCode(upsertSuppliedResult(coll, {"_id.x": [1]}, {}), [
    ErrorCodes.ShardKeyNotFound,
    ErrorCodes.NotSingleValueField
]);  // Shard key cannot contain array values or array descendants.
// With the introduction of PM-1632, we can execute upsert not targeted to a single shard,
// but since shard key fields cannot contain arrays or array descendants, we will still
// throw NotSingleValueField when we go to upsert the document.
assert.commandFailedWithCode(upsertSuppliedResult(coll, {_id: [{x: 1}]}, {}),
                             ErrorCodes.NotSingleValueField);

assert.commandFailedWithCode(
    upsertedResult(coll, {_id: {x: [1]}}, {$set: {y: 1}}),
    ErrorCodes.ShardKeyNotFound);  // Shard key cannot contain array values or array descendants.
assert.commandFailedWithCode(upsertedResult(coll, {"_id.x": [1]}, {$set: {y: 1}}), [
    ErrorCodes.ShardKeyNotFound,
    ErrorCodes.NotSingleValueField
]);  // Shard key cannot contain array values or array descendants.
// With the introduction of PM-1632, we can execute upsert not targeted to a single shard,
// but since shard key fields cannot contain arrays or array descendants, we will still
// throw NotSingleValueField when we go to upsert the document.
assert.commandFailedWithCode(
    upsertedResult(coll, {_id: [{x: 1}]}, {$set: {y: 1}}),
    ErrorCodes.ShardKeyNotFound);  // Shard key cannot contain array values or array descendants.

// Replacement and op-style {$set _id} fail when using dotted-path query on nested _id key.
// TODO SERVER-44615: these tests should succeed when SERVER-44615 is complete.
assert.commandFailedWithCode(upsertedResult(coll, {"_id.x": -1}, {_id: {x: -1}}),
                             ErrorCodes.NotExactValueField);

assert.commandWorked(upsertSuppliedResult(coll, {"_id.x": -1}, {_id: {x: -1}}));
assert.docEq({_id: {x: -1}}, coll.findOne({}));

assert.commandFailedWithCode(upsertedResult(coll, {"_id.x": -1}, {$set: {_id: {x: -1}}}),
                             ErrorCodes.ImmutableField);

assert.commandWorked(upsertedResult(coll, {"_id.x": -1}, {$set: {"_id.x": -1}}));
assert.docEq({_id: {x: -1}}, coll.findOne({}));

st.stop();
