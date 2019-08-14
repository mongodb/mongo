// Verifies mongos will implicitly abort a transaction on all involved shards on a transaction fatal
// error.
//
// @tags: [requires_sharding, uses_transactions, uses_multi_shard_transaction]
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({shards: 2, mongos: 1, config: 1});

// Set up a sharded collection with one chunk on each shard.

assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 1}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

//
// An unhandled error during a transaction should try to abort it on all participants.
//

session.startTransaction();

// Targets Shard0 successfully.
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: -1}}));

// Sharding tests require failInternalCommands: true, since the mongos appears to mongod to be
// an internal client.
assert.commandWorked(st.rs1.getPrimary().adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.InternalError, failCommands: ["find"], failInternalCommands: true}
}));

// Targets Shard1 and encounters a transaction fatal error.
assert.commandFailedWithCode(sessionDB.runCommand({find: collName, filter: {_id: 1}}),
                             ErrorCodes.InternalError);

assert.commandWorked(
    st.rs1.getPrimary().adminCommand({configureFailPoint: "failCommand", mode: "off"}));

// The transaction should have been aborted on both shards.
assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

st.stop();
})();
