/**
 * Tests for the 'metrics.query' section of the mongoS serverStatus response dealing with CRUD
 * operations.
 */

(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

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
assert.commandWorked(testColl.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
assert.commandWorked(testColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

// Should increment the metric because we broadcast by _id, even though the update subsequently
// fails on the individual shard.
assert.commandFailedWithCode(testColl.update({_id: 1}, {$set: {x: 2}}, {multi: false}), 31025);
assert.commandFailedWithCode(testColl.update({_id: 1}, {$set: {x: 12}, $hello: 1}, {multi: false}),
                             ErrorCodes.FailedToParse);

let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

// Verify that the above four updates incremented the metric counter.
assert.eq(4, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);

// Shouldn't increment the metric when {multi:true}.
assert.commandWorked(testColl.update({_id: 1}, {$set: {a: 3}}, {multi: true}));
assert.commandWorked(testColl.update({}, {$set: {a: 3}}, {multi: true}));

// Shouldn't increment the metric when update can target single shard.
assert.commandWorked(testColl.update({x: 11}, {$set: {a: 2}}, {multi: false}));
assert.commandWorked(testColl.update({x: 1}, {$set: {a: 2}}, {multi: false}));

// Shouldn't increment the metric for replacement style updates.
assert.commandWorked(testColl.update({_id: 1}, {x: 1, a: 2}));
assert.commandWorked(testColl.update({x: 1}, {x: 1, a: 1}));

// Sharded deleteOnes that do not directly target a shard can now use the two phase write
// protocol to execute.
if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    const testColl2 = testDB.testColl2;

    // Shard testColl2 on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1. This collection
    // is used to for the update below which would use the write without shard key protocol, but
    // since the query is unspecified, any 1 random document could be modified. In order to not
    // break the state of the original test 'testColl', 'testColl2' is used specifically for the
    // single update below.
    st.shardColl(testColl2, {x: 1}, {x: 0}, {x: 1});

    assert.commandWorked(testColl2.insert({x: 1, _id: 1}));
    assert.commandWorked(testColl2.insert({x: -1, _id: 0}));

    // TODO: SERVER-67429 Remove this try/catch since we can run in all configurations.
    // If we have a WouldChangeOwningShard update and we aren't running as a retryable
    // write or in a transaction, then this is an acceptable error.
    let updateRes;
    try {
        updateRes = testColl2.update({}, {$set: {x: 2}}, {multi: false});
        assert.commandWorked(updateRes);
        assert.eq(1, updateRes.nMatched);
        assert.eq(1, updateRes.nModified);
        assert.eq(testColl2.find({x: 2}).itcount(), 1);
    } catch (e) {
        // If a WouldChangeOwningShard update is performed not as a retryable write or in a
        // transaction, expect an error.
        assert.eq(updateRes.getWriteError().code, ErrorCodes.IllegalOperation);
        assert(updateRes.getWriteError().errmsg.includes(
            "Must run update to shard key field in a multi-statement transaction or with " +
            "retryWrites: true."));
    }

    // Shouldn't increment the metrics for unsharded collection.
    assert.commandWorked(unshardedColl.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
    assert.commandWorked(unshardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

    // Shouldn't increment the metrics when query had invalid operator.
    assert.commandFailedWithCode(
        testColl.update({_id: 1, $invalidOperator: 1}, {$set: {a: 2}}, {multi: false}),
        ErrorCodes.BadValue);

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // TODO: SERVER-69810 ServerStatus metrics for tracking number of
    // updateOnes/deleteOnes/findAndModifies
    // assert.eq(5, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);
} else {
    // Shouldn't increment the metric when routing fails.
    assert.commandFailedWithCode(testColl.update({}, {$set: {x: 2}}, {multi: false}),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(testColl.update({_id: 1}, {$set: {x: 2}}, {upsert: true}),
                                 ErrorCodes.ShardKeyNotFound);

    // Shouldn't increment the metrics for unsharded collection.
    assert.commandWorked(unshardedColl.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
    assert.commandWorked(unshardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

    // Shouldn't incement the metrics when query had invalid operator.
    assert.commandFailedWithCode(
        testColl.update({_id: 1, $invalidOperator: 1}, {$set: {a: 2}}, {multi: false}),
        ErrorCodes.BadValue);

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verify that only the first four upserts incremented the metric counter.
    assert.eq(4, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);
}

st.stop();
})();
