/**
 * Tests for the 'metrics.query' section of the mongoS serverStatus response dealing with CRUD
 * operations.
 */

(function() {
"use strict";

const st = new ShardingTest({shards: 2});
const testDB = st.s.getDB("test");
const testColl = testDB.coll;
const unshardedColl = testDB.unsharded;

assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

// Shard testColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
st.shardColl(testColl, {x: 1}, {x: 0}, {x: 1});

// Insert one document on each shard.
assert.commandWorked(testColl.insert({x: 1, _id: 1}));
assert.commandWorked(testColl.insert({x: -1, _id: 0}));

assert.commandWorked(unshardedColl.insert({x: 1, _id: 1}));

// Verification for 'updateOneOpStyleBroadcastWithExactIDCount' metric.

// Should increment the metric as the update cannot target single shard and are {multi:false}.
assert.commandWorked(testDB.coll.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
assert.commandWorked(testDB.coll.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

// Should increment the metric because we broadcast by _id, even though the update subsequently
// fails on the individual shard.
assert.commandFailedWithCode(testDB.coll.update({_id: 1}, {$set: {x: 2}}, {multi: false}),
                             [ErrorCodes.ImmutableField, 31025]);
assert.commandFailedWithCode(
    testDB.coll.update({_id: 1}, {$set: {x: 2, $invalidField: 4}}, {multi: false}),
    ErrorCodes.DollarPrefixedFieldName);

let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

// Verify that the above four updates incremented the metric counter.
assert.eq(4, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);

// Shouldn't increment the metric when {multi:true}.
assert.commandWorked(testDB.coll.update({_id: 1}, {$set: {a: 3}}, {multi: true}));
assert.commandWorked(testDB.coll.update({}, {$set: {a: 3}}, {multi: true}));

// Shouldn't increment the metric when update can target single shard.
assert.commandWorked(testDB.coll.update({x: 11}, {$set: {a: 2}}, {multi: false}));
assert.commandWorked(testDB.coll.update({x: 1}, {$set: {a: 2}}, {multi: false}));

// Shouldn't increment the metric for replacement style updates.
assert.commandWorked(testDB.coll.update({_id: 1}, {x: 1, a: 2}));
assert.commandWorked(testDB.coll.update({x: 1}, {x: 1, a: 1}));

// Shouldn't increment the metric when routing fails.
assert.commandFailedWithCode(testDB.coll.update({}, {$set: {x: 2}}, {multi: false}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(testDB.coll.update({_id: 1}, {$set: {x: 2}}, {upsert: true}),
                             ErrorCodes.ShardKeyNotFound);

// Shouldn't increment the metrics for unsharded collection.
assert.commandWorked(unshardedColl.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
assert.commandWorked(unshardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

// Shouldn't incement the metrics when query had invalid operator.
assert.commandFailedWithCode(
    testDB.coll.update({_id: 1, $invalidOperator: 1}, {$set: {a: 2}}, {multi: false}),
    ErrorCodes.BadValue);

mongosServerStatus = testDB.adminCommand({serverStatus: 1});

// Verify that only the first four upserts incremented the metric counter.
assert.eq(4, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);

st.stop();
})();
