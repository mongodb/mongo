// Verifies multi-writes in transactions are sent with shard versions to only the targeted shards.
//
// @tags: [
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

const st = new ShardingTest({shards: 3, config: 1, mongos: 2});

enableStaleVersionAndSnapshotRetriesWithinTransactions(st);

// Set up a sharded collection with 3 chunks, [min, 0), [0, 10), [10, max), one on each shard,
// with one document in each.

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 10}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 5}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 15}, to: st.shard2.shardName}));

assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 1, counter: 0, skey: -5}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 2, counter: 0, skey: 5}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 3, counter: 0, skey: 15}));

// Runs the given multi-write and asserts a manually inserted orphan document is not affected.
// The write is assumed to target chunks [min, 0) and [0, 10), which begin on shard0 and shard1,
// respectively.
function runTest(st, session, writeCmd, staleRouter) {
    const isUpdate = writeCmd.hasOwnProperty("update");
    const sessionDB = session.getDatabase(dbName);

    let orphanShardName;
    let orphanDoc = {_id: 2, counter: 0, skey: 5};
    if (staleRouter) {
        // Using a separate router, move a chunk that will be targeted by the write to a shard
        // that would not be targeted by a stale router. Put the orphan on the shard that
        // previously owned the chunk to verify the multi-write obeys the shard versioning
        // protocol.
        assert.commandWorked(st.s1.adminCommand(
            {moveChunk: ns, find: {skey: 5}, to: st.shard2.shardName, _waitForDelete: true}));
        orphanShardName = "rs1";
    } else {
        // Otherwise put the orphan on a shard that should not be targeted by a fresh router to
        // verify the multi-write is not broadcast to all shards.
        orphanShardName = "rs2";
    }

    const orphanShardDB = st[orphanShardName].getPrimary().getDB(dbName);
    assert.commandWorked(
        orphanShardDB[collName].insert(orphanDoc, {writeConcern: {w: "majority"}}));

    // Start a transaction with majority read concern to ensure the orphan will be visible if
    // its shard is targeted and send the multi-write.
    session.startTransaction({readConcern: {level: "majority"}});
    assert.commandWorked(sessionDB.runCommand(writeCmd));

    // The write shouldn't be visible until the transaction commits.
    assert.sameMembers(st.getDB(dbName)[collName].find().toArray(), [
        {_id: 1, counter: 0, skey: -5},
        {_id: 2, counter: 0, skey: 5},
        {_id: 3, counter: 0, skey: 15}
    ]);

    // Commit the transaction and verify the write was successful.
    assert.commandWorked(session.commitTransaction_forTesting());
    if (isUpdate) {
        assert.sameMembers(
            st.getDB(dbName)[collName].find().toArray(),
            [
                {_id: 1, counter: 1, skey: -5},
                {_id: 2, counter: 1, skey: 5},
                {_id: 3, counter: 0, skey: 15}
            ],
            "document mismatch for update, stale: " + staleRouter + ", cmd: " + tojson(writeCmd));
    } else {  // isDelete
        assert.sameMembers(
            st.getDB(dbName)[collName].find().toArray(),
            [{_id: 3, counter: 0, skey: 15}],
            "document mismatch for delete, stale: " + staleRouter + ", cmd: " + tojson(writeCmd));
    }

    // The orphaned document should not have been affected.
    assert.docEq(
        orphanDoc,
        orphanShardDB[collName].findOne({skey: orphanDoc.skey}),
        "document mismatch for orphaned doc, stale: " + staleRouter + ", cmd: " + tojson(writeCmd));

    // Reset the database state for the next iteration.
    if (isUpdate) {
        assert.commandWorked(sessionDB[collName].update({}, {$set: {counter: 0}}, {multi: true}));
    } else {  // isDelete
        assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 1, counter: 0, skey: -5}));
        assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 2, counter: 0, skey: 5}));
    }

    assert.commandWorked(orphanShardDB[collName].remove({skey: orphanDoc.skey}));

    if (staleRouter) {
        // Move the chunk back with the main router so it isn't stale.
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: ns, find: {skey: 5}, to: st.shard1.shardName, _waitForDelete: true}));
    }
}

const session = st.s.startSession();

let multiUpdate = {
    update: collName,
    updates: [{q: {skey: {$lte: 5}}, u: {$inc: {counter: 1}}, multi: true}]
};

multiUpdate.ordered = false;
runTest(st, session, multiUpdate, false /*staleRouter*/);
// TODO: SERVER-39704 uncomment when mongos can internally retry txn on stale errors for real.
// runTest(st, session, multiUpdate, true /*staleRouter*/);

multiUpdate.ordered = true;
runTest(st, session, multiUpdate, false /*staleRouter*/);
// TODO: SERVER-39704 uncomment when mongos can internally retry txn on stale errors for real.
// runTest(st, session, multiUpdate, true /*staleRouter*/);

let multiDelete = {delete: collName, deletes: [{q: {skey: {$lte: 5}}, limit: 0}]};

multiDelete.ordered = false;
runTest(st, session, multiDelete, false /*staleRouter*/);
// TODO: SERVER-39704 uncomment when mongos can internally retry txn on stale errors for real.
// runTest(st, session, multiDelete, true /*staleRouter*/);

multiDelete.ordered = true;
runTest(st, session, multiDelete, false /*staleRouter*/);
// TODO: SERVER-39704 uncomment when mongos can internally retry txn on stale errors for real.
// runTest(st, session, multiDelete, true /*staleRouter*/);

disableStaleVersionAndSnapshotRetriesWithinTransactions(st);

st.stop();
})();
