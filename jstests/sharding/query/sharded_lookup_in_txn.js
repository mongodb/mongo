/**
 * Tests that $lookup and $graphLookup against a sharded collection are banned in a transaction.
 * @tags: [
 *   featureFlagShardedLookup,
 *   # Prior to FCV 5.1, these commands will fail with a different error code because $lookup and
 *   # $graphLookup against sharded collections were banned.
 *   requires_fcv_51,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

const st = new ShardingTest({shards: 2, mongos: 1});
const testName = "sharded_lookup_in_txn";

const mongosDB = st.s0.getDB(testName);
assert.commandWorked(mongosDB.dropDatabase());

// Shard the foreign collection on _id.
const fromColl = mongosDB.from;
st.shardColl(fromColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

assert.commandWorked(fromColl.insert({_id: 0, b: 1}));
assert.commandWorked(fromColl.insert({_id: 1, b: 2}));

assert.commandWorked(mongosDB.local.insert({_id: 0, a: 1}));
assert.commandWorked(mongosDB.local.insert({_id: 1, a: 2}));

function assertFailsInTransaction(pipeline, errorCode) {
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(testName);

    session.startTransaction();
    assert.throwsWithCode(() => sessionDB.local.aggregate(pipeline).itcount(), errorCode);
    session.endSession();
}

// $lookup and $graphLookup against a sharded foreign collection in a transaction should fail even
// when featureFlagShardedLookup is enabled.
assertFailsInTransaction([{
    $lookup: {
        from: fromColl.getName(),
        localField: "a",
        foreignField: "b",
        as: "res",
    }
}],
3904800);

assertFailsInTransaction([{
    $graphLookup: {
        from: fromColl.getName(),
        startWith: "$a",
        connectFromField: "b",
        connectToField: "_id",
        as: "res"
    }
}],
3904801);

st.stop();
}());
