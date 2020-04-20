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

const dbName = "TestDB";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
});

// Set up one sharded collection with 2 chunks distributd across 2 shards
assert.commandWorked(st.enableSharding(dbName, st.shard0.shardName));
assert.commandWorked(st.s.getDB(dbName).runCommand({
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"}
}));

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));

assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -5}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

const session = st.s.getDB(dbName).getMongo().startSession({causalConsistency: false});
let sessionDB = session.getDatabase(dbName);
let sessionColl = sessionDB[collName];

{
    jsTest.log("Testing createIndexes on an existing index in a transaction");
    session.startTransaction({writeConcern: {w: "majority"}});

    assert.commandWorked(
        sessionColl.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}));
    // Perform cross-shard writes to execute prepare path.
    assert.commandWorked(sessionColl.insert({_id: -1, m: -1}));
    assert.commandWorked(sessionColl.insert({_id: +1, m: +1}));
    assert.eq(-1, sessionColl.findOne({m: -1})._id);
    assert.eq(+1, sessionColl.findOne({m: +1})._id);

    session.commitTransaction();
}
{
    jsTest.log(
        "Testing createIndexes on an existing index in a transaction when not all shards are" +
        " aware of that index (should abort)");

    // Simulate a scenario where one shard with chunks for a collection is unaware of one of the
    // collection's indexes
    assert.commandWorked(st.shard1.getDB(dbName).getCollection(collName).dropIndexes("a_1"));

    session.startTransaction({writeConcern: {w: "majority"}});

    assert.commandFailedWithCode(
        sessionColl.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}),
        ErrorCodes.OperationNotSupportedInTransaction);

    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
}

// Resolve index inconsistency to pass consistency checks.
st.shard1.getDB(dbName).getCollection(collName).runCommand(
    {createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]});

st.stop();
})();
