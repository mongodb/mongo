// Verifies targeting errors encountered in a transaction lead to write errors when write without
// shard key feature is not enabled.
//
// @tags: [uses_transactions]
(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: "hashed"}}));

const session = st.s.startSession();
const sessionDB = session.getDatabase("test");

if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(sessionDB)) {
    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand(
        {update: collName, updates: [{q: {skey: {$lte: 5}}, u: {$set: {x: 1}}, multi: false}]}));
    assert.commandWorked(session.abortTransaction_forTesting());

    session.startTransaction();
    assert.commandWorked(
        sessionDB.runCommand({delete: collName, deletes: [{q: {skey: {$lte: 5}}, limit: 1}]}));
    assert.commandWorked(session.abortTransaction_forTesting());

    st.stop();
    return;
}

// Failed update.

session.startTransaction();

let res = sessionDB.runCommand(
    {update: collName, updates: [{q: {skey: {$lte: 5}}, u: {$set: {x: 1}}, multi: false}]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
assert(res.hasOwnProperty("writeErrors"), "expected write errors, res: " + tojson(res));

assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

// Failed delete.

session.startTransaction();

res = sessionDB.runCommand({delete: collName, deletes: [{q: {skey: {$lte: 5}}, limit: 1}]});
assert.commandFailedWithCode(res, ErrorCodes.ShardKeyNotFound);
assert(res.hasOwnProperty("writeErrors"), "expected write errors, res: " + tojson(res));

assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

st.stop();
}());
