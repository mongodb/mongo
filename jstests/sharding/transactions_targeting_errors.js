// Verifies targeting errors encountered in a transaction lead to write errors.
//
// @tags: [uses_transactions]
(function() {
"use strict";

// TODO: SERVER-72438 Change transaction_targeting_errors.js to validate writeErrors that aren't due
// to shard key targeting.
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
