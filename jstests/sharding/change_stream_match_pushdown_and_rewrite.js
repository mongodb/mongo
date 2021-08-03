// Test that a pipeline of the form [{$changeStream: {}}, {$match: ...}] can rewrite the $match and
// apply it to oplog-format documents in order to filter out results as early as possible.
// @tags: [
//   featureFlagChangeStreamsRewrite,
//   requires_fcv_51,
//   requires_pipeline_optimization,
//   requires_sharding,
//   uses_change_streams,
// ]
(function() {
"use strict";

const dbName = "test";
const collName = "change_stream_match_pushdown_and_rewrite_and_rewrite";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

st.rs0.nodes.forEach(function(node) {
    assert(node.getDB(dbName).setProfilingLevel(2));
});

st.rs1.nodes.forEach(function(node) {
    assert(node.getDB(dbName).setProfilingLevel(2));
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

const changeStreamComment = "change_stream_match_pushdown_and_rewrite change stream";
const changeStream = coll.aggregate([{$changeStream: {}}, {$match: {operationType: "insert"}}],
                                    {comment: changeStreamComment});

// These commands will result in 6 oplog events, with all but 2 will be filtered out by the $match.
assert.commandWorked(coll.insert({shard: 1}));
assert.commandWorked(coll.update({shard: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.remove({shard: 1}));
assert.commandWorked(coll.insert({shard: 2}));
assert.commandWorked(coll.update({shard: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.remove({shard: 2}));

// Verify correct operation of the change stream.
assert.soon(() => changeStream.hasNext());
const event1 = changeStream.next();
assert.eq(event1.operationType, "insert", event1);
assert.eq(event1.fullDocument.shard, 1, event1);

assert.soon(() => changeStream.hasNext());
const event2 = changeStream.next();
assert.eq(event2.operationType, "insert", event2);
assert.eq(event2.fullDocument.shard, 2, event2);

assert(!changeStream.hasNext());
changeStream.close();

function assertNumChangeStreamDocsReturnedFromShard(shardReplSet, comment, expectedTotalReturned) {
    const result = shardReplSet.getPrimary()
                       .getDB(dbName)
                       .getCollection("system.profile")
                       .aggregate([
                           {$match: {op: "getmore", "originatingCommand.comment": comment}},
                           {$group: {_id: null, totalReturned: {$sum: "$nreturned"}}}
                       ])
                       .toArray();

    assert.eq(result.length, 1, result);
    assert.eq(result[0].totalReturned, expectedTotalReturned, result[0]);
}

// Verify the number of documents seen from each shard by the mongoS pipeline. Because we expect the
// $match to be pushed down to the shards, we expect to only see the 1 "insert" operation on each
// shard. All other operations should be filtered out on the shards.
assertNumChangeStreamDocsReturnedFromShard(st.rs0, changeStreamComment, 1);
assertNumChangeStreamDocsReturnedFromShard(st.rs1, changeStreamComment, 1);

// Helper to extract the 'executionStats' from the $cursor pipeline stage.
function getOplogExecutionStatsForShard(stats, shardName) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    assert.eq(Object.keys(stats.shards[shardName].stages[0])[0], "$cursor", stats);
    return stats.shards[shardName].stages[0].$cursor.executionStats;
}

// Run the same change stream again, this time with "executionStats" to get more detailed
// information about how many documents are processed by each stage. Note that 'event1' will not be
// included in the results set this time, because we are using it as the resume point, but will
// still be returned from the shard and get processed on the mongoS.
const stats = coll.explain("executionStats").aggregate([
    {$changeStream: {resumeAfter: event1._id}},
    {$match: {operationType: "insert"}}
]);

// Because it is possible to rewrite the {operationType: "insert"} predicate so that it applies to
// the oplog entry, we expect the $match to get pushed all the way to the initial oplog query. This
// query executes in an internal "$cursor" stage, and we expect to see exactly 1 document from this
// stage on each shard.
const execStatsShard0 = getOplogExecutionStatsForShard(stats, st.rs0.name);
assert.eq(execStatsShard0.nReturned, 1, execStatsShard0);

const execStatsShard1 = getOplogExecutionStatsForShard(stats, st.rs1.name);
assert.eq(execStatsShard1.nReturned, 1, execStatsShard1);

// Generate another 6 oplog events, this time within transactions.
const session = st.s.startSession({causalConsistency: true});
const sessionColl = session.getDatabase(dbName)[collName];

session.startTransaction({readConcern: {level: "majority"}});

assert.commandWorked(sessionColl.insert({shard: 1}));
assert.commandWorked(sessionColl.update({shard: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({shard: 1}));
assert.commandWorked(sessionColl.insert({shard: 2}));
assert.commandWorked(sessionColl.update({shard: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({shard: 2}));

assert.commandWorked(session.commitTransaction_forTesting());

// Repeat the change stream from before, using a resume token to pick up from where the previous
// change stream left off. This change stream will only observe the 6 operations that occur in the
// transaction and will filter out everything except the 2 inserts.
const txnChangeStreamComment =
    "change_stream_match_pushdown_and_rewrite change stream for transaction";
const txnChangeStream = coll.aggregate(
    [{$changeStream: {resumeAfter: event2._id}}, {$match: {operationType: "insert"}}],
    {comment: txnChangeStreamComment});

// Verify correct operation of the change stream.
assert.soon(() => txnChangeStream.hasNext());
const event3 = txnChangeStream.next();
assert.eq(event3.operationType, "insert", event3);
assert.eq(event3.fullDocument.shard, 1, event3);

assert.soon(() => txnChangeStream.hasNext());
const event4 = txnChangeStream.next();
assert.eq(event4.operationType, "insert", event4);
assert.eq(event4.fullDocument.shard, 2, event4);

assert(!txnChangeStream.hasNext());
txnChangeStream.close();

// Verify the number of documents seen from each shard by the mongoS pipeline. As before, we expect
// that everything except the inserts will be filtered on the shard, limiting the number of events
// the mongoS needs to retrieve.
assertNumChangeStreamDocsReturnedFromShard(st.rs0, txnChangeStreamComment, 1);

// Note that the event we are resuming from is sent to the mongoS from shard 2, even though it gets
// filtered out, which is why we see 2 events here.
assertNumChangeStreamDocsReturnedFromShard(st.rs1, txnChangeStreamComment, 2);

// Generate a second transaction.
session.startTransaction({readConcern: {level: "majority"}});

assert.commandWorked(sessionColl.insert({shard: 1, a: 1}));
assert.commandWorked(sessionColl.update({shard: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({shard: 1}));
assert.commandWorked(sessionColl.insert({shard: 2, a: 2}));
assert.commandWorked(sessionColl.update({shard: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({shard: 2}));

assert.commandWorked(session.commitTransaction_forTesting());

// This change stream targets transactions from this session but filters out the first transaction.
const txnStats = coll.explain("executionStats").aggregate([
    {$changeStream: {resumeAfter: event1._id}},
    {$match: {operationType: "insert", lsid: event3.lsid, txnNumber: {$ne: event3.txnNumber}}}
]);

// The "lsid" and "txnNumber" filters should get pushed all the way to the initial oplog query in
// the $cursor stage, meaning that every oplog entry gets filtered out except the
// 'commitTransaction' on each shard for the one transaction we select with our filter.
const txnExecStatsShard0 = getOplogExecutionStatsForShard(txnStats, st.rs0.name);
assert.eq(txnExecStatsShard0.nReturned, 1, txnExecStatsShard0);

const txnExecStatsShard1 = getOplogExecutionStatsForShard(txnStats, st.rs1.name);
assert.eq(txnExecStatsShard1.nReturned, 1, txnExecStatsShard1);

st.stop();
})();
