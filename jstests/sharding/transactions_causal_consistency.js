// Verifies basic sharded transaction behavior with causal consistency.
//
// @tags: [
//   requires_find_command,
//   requires_sharding,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2, mongos: 2});

enableStaleVersionAndSnapshotRetriesWithinTransactions(st);

// Set up a sharded collection with 2 chunks, [min, 0) and [0, max), one on each shard, with one
// document in each.

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));

assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 1}, {writeConcern: {w: "majority"}}));

// Verifies transactions using causal consistency read all causally prior operations.
function runTest(st, readConcern) {
    jsTestLog("Testing readConcern: " + tojson(readConcern));

    const session = st.s.startSession({causalConsistency: true});
    const sessionDB = session.getDatabase(dbName);

    // Insert data to one shard in a causally consistent session.
    const docToInsert = {_id: 5};
    assert.commandWorked(sessionDB.runCommand({insert: collName, documents: [docToInsert]}));

    // Through a separate router move the chunk that was inserted to, so the original router is
    // stale when it starts its transaction.
    const otherRouter = st.s1;
    assert.commandWorked(
        otherRouter.adminCommand({moveChunk: ns, find: docToInsert, to: st.shard0.shardName}));

    session.startTransaction({readConcern: readConcern});

    // The transaction should always see the document written earlier through its session,
    // regardless of the move.
    //
    // Note: until transactions can read from secondaries and/or disabling speculative snapshot
    // is allowed, read concerns that do not require global snapshots (i.e. local and majority)
    // will always read the inserted document here because the local snapshot established on
    // this shard will include all currently applied operations, which must include all earlier
    // acknowledged writes.
    assert.docEq(docToInsert,
                 sessionDB[collName].findOne(docToInsert),
                 "sharded transaction with read concern " + tojson(readConcern) +
                     " did not see expected document");

    assert.commandWorked(session.commitTransaction_forTesting());

    // Clean up for the next iteration.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: docToInsert, to: st.shard1.shardName}));
    assert.commandWorked(sessionDB[collName].remove(docToInsert));
}

const kAllowedReadConcernLevels = ["local", "majority", "snapshot"];
for (let readConcernLevel of kAllowedReadConcernLevels) {
    runTest(st, {level: readConcernLevel});
}

disableStaleVersionAndSnapshotRetriesWithinTransactions(st);

st.stop();
})();
