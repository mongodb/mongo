// Confirms that change streams correctly handle prepared transactions with an empty applyOps entry.
// This test creats a multi-shard transaction in which one of the participating shards has only a
// no-op write, resulting in the empty applyOps scenario we wish to test. Exercises the fix for
// SERVER-50769.
// @tags: [
//   requires_sharding,
//   uses_change_streams,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
"use strict";

const dbName = "test";
const collName = "change_stream_empty_apply_ops";
const namespace = dbName + "." + collName;

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(coll.createIndex({shard: 1}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
// Shard the test collection and split it into two chunks: one that contains all {shard: 1}
// documents and one that contains all {shard: 2} documents.
st.shardColl(collName,
             {shard: 1} /* shard key */,
             {shard: 2} /* split at */,
             {shard: 2} /* move the chunk containing {shard: 2} to its own shard */,
             dbName,
             true);
// Seed each chunk with an initial document.
assert.commandWorked(coll.insert({shard: 1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(coll.insert({shard: 2}, {writeConcern: {w: "majority"}}));

// Open up change streams.
const changeStreamCursorColl = coll.watch();
const changeStreamCursorDB = db.watch();
const changeStreamCursorCluster = mongosConn.watch();

// Start a transaction, which will include both shards.
const sesion = db.getMongo().startSession({causalConsistency: true});
const sessionDb = sesion.getDatabase(dbName);
const sessionColl = sessionDb[collName];

sesion.startTransaction({readConcern: {level: "majority"}});

// This no-op will make one of the shards a transaction participant without generating an actual
// write. The transaction will send an empty prepared transaction to the shard, in the form of an
// applyOps command with no operations.
sessionColl.findAndModify({query: {shard: 1}, update: {$setOnInsert: {a: 1}}});

// This write, which is not a no-op, occurs on the other shard.
sessionColl.findAndModify({query: {shard: 2}, update: {$set: {a: 1}}});

assert.commandWorked(sesion.commitTransaction_forTesting());

// Each change stream should see exactly one update, resulting from the valid write on shard 2.
[changeStreamCursorColl, changeStreamCursorDB, changeStreamCursorCluster].forEach(function(
    changeStreamCursor) {
    assert.soon(() => changeStreamCursor.hasNext());
    const changeDoc = changeStreamCursor.next();
    assert.eq(changeDoc.documentKey.shard, 2);
    assert.eq(changeDoc.operationType, "update");

    assert(!changeStreamCursor.hasNext());
});

st.stop();
})();
