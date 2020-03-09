// Test that ensuring index existence (createIndexes on an existing index) successfully runs in a
// cross-shard transaction and that attempting to do createIndexes when only a subset of shards is
// aware of the existing index fails.
//
// @tags: [
//   requires_find_command,
//   requires_fcv_44,
//   requires_sharding,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
"use strict";

function expectChunks(st, ns, chunks) {
    for (let i = 0; i < chunks.length; i++) {
        assert.eq(chunks[i],
                  st.s.getDB("config").chunks.count({ns: ns, shard: st["shard" + i].shardName}),
                  "unexpected number of chunks on shard " + i);
    }
}

const dbName = "test";
const dbNameShard2 = "testOther";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({
    shards: 3,
    mongos: 1,
});

// Set up one sharded collection with 2 chunks, both on the primary shard.

assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -5}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.getDB(dbName).runCommand({
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"}
}));

// Set up another collection with a different shard (shard 2) as its primary shard.
assert.commandWorked(
    st.s.getDB(dbNameShard2)[collName].insert({_id: 4}, {writeConcern: {w: "majority"}}));
st.ensurePrimaryShard(dbNameShard2, st.shard2.shardName);

const session = st.s.getDB(dbName).getMongo().startSession({causalConsistency: false});

let sessionDB = session.getDatabase(dbName);
let sessionColl = sessionDB[collName];
let sessionDBShard2 = session.getDatabase(dbNameShard2);
let sessionCollShard2 = sessionDBShard2[collName];

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

expectChunks(st, ns, [2, 0, 0]);

st.stopBalancer();

// Ensure collection `ns` has chunks distributed across two shards.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
expectChunks(st, ns, [1, 1, 0]);

// Ensure no stale version errors occur.
let doc = st.s.getDB(dbName).getCollection(collName).findOne({_id: 5});
assert.eq(doc._id, 5);
let doc2 = st.s.getDB(dbNameShard2).getCollection(collName).findOne({_id: 4});
assert.eq(doc2._id, 4);

jsTest.log("Testing createIndexes on an existing index in a transaction");
session.startTransaction({writeConcern: {w: "majority"}});

assert.commandWorked(
    sessionColl.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}));
// Perform cross-shard writes to execute prepare path.
assert.commandWorked(sessionColl.insert({n: 2}));
assert.commandWorked(sessionCollShard2.insert({m: 1}));
assert.eq(sessionCollShard2.findOne({m: 1}).m, 1);
assert.eq(sessionColl.findOne({n: 2}).n, 2);

session.commitTransaction();

jsTest.log("Testing createIndexes on an existing index in a transaction when not all shards are" +
           " aware of that index (should abort)");

// Simulate a scenario where one shard with chunks for a collection is unaware of one of the
// collection's indexes.
st.shard1.getDB(dbName).getCollection(collName).dropIndexes("a_1");

session.startTransaction({writeConcern: {w: "majority"}});

assert.commandFailedWithCode(
    sessionColl.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}),
    ErrorCodes.OperationNotSupportedInTransaction);

assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

// Resolve index inconsistency to pass consistency checks.
st.shard1.getDB(dbName).getCollection(collName).runCommand(
    {createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]});

st.startBalancer();

st.stop();
})();
