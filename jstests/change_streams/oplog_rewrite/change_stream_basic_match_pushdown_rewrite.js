// Test that a pipeline of the form [{$changeStream: {}}, {$match: ...}] can rewrite the $match and
// apply it to oplog-format documents in order to filter out results as early as possible.
// @tags: [
//   featureFlagChangeStreamsRewrite,
//   requires_fcv_51,
//   requires_pipeline_optimization,
//   requires_sharding,
//   uses_change_streams,
//   change_stream_does_not_expect_txns,
//   assumes_unsharded_collection,
//   assumes_read_preference_unchanged
// ]
(function() {
"use strict";

load("jstests/libs/change_stream_rewrite_util.js");  // For rewrite helpers.

const dbName = "change_stream_match_pushdown_and_rewrite";
const collName = "coll1";
const collNameAlternate = "change_stream_match_pushdown_and_rewrite_alternate";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

// Create a sharded collection.
const coll = createShardedCollection(st, "_id" /* shardKey */, dbName, collName, 2 /* splitAt */);

// Create a second (unsharded) test collection for validating transactions that insert into multiple
// collections.
assert.commandWorked(db.createCollection(collNameAlternate));

const changeStream = coll.aggregate([{$changeStream: {}}, {$match: {operationType: "insert"}}]);

// Store a resume token that can be used to start the change stream from the beginning.
const resumeAfterToken = changeStream.getResumeToken();

// These commands will result in 6 oplog events, with all but 2 will be filtered out by the
// $match.
assert.commandWorked(coll.insert({_id: 1, string: "Value"}));
assert.commandWorked(coll.update({_id: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.remove({_id: 1}));
assert.commandWorked(coll.insert({_id: 2, string: "vAlue"}));
assert.commandWorked(coll.update({_id: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.remove({_id: 2}));

// Verify correct operation of the change stream.
assert.soon(() => changeStream.hasNext());
const event1 = changeStream.next();
assert.eq(event1.operationType, "insert", event1);
assert.eq(event1.documentKey._id, 1, event1);

assert.soon(() => changeStream.hasNext());
const event2 = changeStream.next();
assert.eq(event2.operationType, "insert", event2);
assert.eq(event2.documentKey._id, 2, event2);

assert(!changeStream.hasNext());
changeStream.close();

// Run the same change stream again, this time with "executionStats" to get more detailed
// information about how many documents are processed by each stage. Note that 'event1' will not
// be included in the results set this time, because we are using it as the resume point, but
// will still be returned from the shard and get processed on the mongoS.
const stats = coll.explain("executionStats").aggregate([
    {$changeStream: {resumeAfter: event1._id}},
    {$match: {operationType: "insert"}}
]);

// Verify the number of documents seen from each shard by the mongoS pipeline. Because we expect
// the $match to be pushed down to the shards, we expect to only see the 1 "insert" operation on
// each shard. All other operations should be filtered out on the shards.
assertNumChangeStreamDocsReturnedFromShard(stats, st.shard0.shardName, 1);
assertNumChangeStreamDocsReturnedFromShard(stats, st.shard1.shardName, 1);

// Because it is possible to rewrite the {operationType: "insert"} predicate so that it applies
// to the oplog entry, we expect the $match to get pushed all the way to the initial oplog
// query. This query executes in an internal "$cursor" stage, and we expect to see exactly 1
// document from this stage on each shard.
assertNumMatchingOplogEventsForShard(stats, st.shard0.shardName, 1);
assertNumMatchingOplogEventsForShard(stats, st.shard1.shardName, 1);

// Generate another 7 oplog events, this time within a transaction. One of the events is in a
// different collection, to validate that events from outside the watched namespace get filtered
// out even when within a transaction.
const session = st.s.startSession({causalConsistency: true});
const sessionColl = session.getDatabase(dbName)[collName];
const sessionCollAlternate = session.getDatabase(dbName)[collNameAlternate];

session.startTransaction({readConcern: {level: "majority"}});

assert.commandWorked(sessionColl.insert({_id: 1}));
assert.commandWorked(sessionColl.update({_id: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({_id: 1}));
assert.commandWorked(sessionCollAlternate.insert({_id: "alt"}));
assert.commandWorked(sessionColl.insert({_id: 2}));
assert.commandWorked(sessionColl.update({_id: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({_id: 2}));

assert.commandWorked(session.commitTransaction_forTesting());

// Repeat the change stream from before, using a resume token to pick up from where the previous
// change stream left off. This change stream will only observe the 6 operations that occur in the
// transaction and will filter out everything except the 2 inserts.
const txnChangeStream = coll.aggregate(
    [{$changeStream: {resumeAfter: event2._id}}, {$match: {operationType: "insert"}}]);

// Verify correct operation of the change stream.
assert.soon(() => txnChangeStream.hasNext());
const event3 = txnChangeStream.next();
assert.eq(event3.operationType, "insert", event3);

assert.soon(() => txnChangeStream.hasNext());
const event4 = txnChangeStream.next();
assert.eq(event4.operationType, "insert", event4);

// Note that the stream may output the two inserts in either order. Because they are within a
// transaction, they effectively occur at exactly the same time.
assert.sameMembers([1, 2], [event3.documentKey._id, event4.documentKey._id], [event3, event4]);

assert(!txnChangeStream.hasNext());
txnChangeStream.close();

// Run explain on the change stream to get more detailed execution information.
const txnStatsAfterEvent2 = coll.explain("executionStats").aggregate([
    {$changeStream: {resumeAfter: event2._id}},
    {$match: {operationType: "insert"}}
]);

// Verify the number of documents seen from each shard by the mongoS pipeline. As before, we expect
// that everything except the inserts will be filtered on the shard, limiting the number of events
// the mongoS needs to retrieve.
assertNumChangeStreamDocsReturnedFromShard(txnStatsAfterEvent2, st.shard0.shardName, 1);

// Note that the event we are resuming from is sent to the mongoS from shard 2, even though it gets
// filtered out, which is why we see 2 events here.
assertNumChangeStreamDocsReturnedFromShard(txnStatsAfterEvent2, st.shard1.shardName, 2);

// Generate a second transaction.
session.startTransaction({readConcern: {level: "majority"}});

assert.commandWorked(sessionColl.insert({_id: 1, a: 1, string: "vaLue"}));
assert.commandWorked(sessionColl.update({_id: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({_id: 1}));
assert.commandWorked(sessionColl.insert({_id: 2, a: 2, string: "valUe"}));
assert.commandWorked(sessionColl.update({_id: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(sessionColl.remove({_id: 2}));

assert.commandWorked(session.commitTransaction_forTesting());

// This change stream targets transactions from this session but filters out the first transaction.
const txnStatsAfterEvent1 = coll.explain("executionStats").aggregate([
    {$changeStream: {resumeAfter: event1._id}},
    {$match: {operationType: "insert", lsid: event3.lsid, txnNumber: {$ne: event3.txnNumber}}}
]);

// The "lsid" and "txnNumber" filters should get pushed all the way to the initial oplog query
// in the $cursor stage, meaning that every oplog entry gets filtered out except the
// 'commitTransaction' on each shard for the one transaction we select with our filter.
assertNumMatchingOplogEventsForShard(txnStatsAfterEvent1, st.shard0.shardName, 1);
assertNumMatchingOplogEventsForShard(txnStatsAfterEvent1, st.shard1.shardName, 1);

// Ensure that optimization does not attempt to create a filter that disregards the collation.
const collationChangeStream = coll.aggregate(
    [{$changeStream: {resumeAfter: resumeAfterToken}}, {$match: {"fullDocument.string": "value"}}],
    {collation: {locale: "en_US", strength: 2}});

let stringValues = [];
for (let i = 0; i < 4; ++i) {
    assert.soon(() => collationChangeStream.hasNext());
    stringValues.push(collationChangeStream.next().fullDocument.string);
}
assert(!collationChangeStream.hasNext());
collationChangeStream.close();

assert.eq(stringValues.slice(0, 2), ["Value", "vAlue"]);

// Again, the stream may output these two inserts in either order. Because they are within a
// transaction, they effectively occur at exactly the same time.
assert.sameMembers(stringValues.slice(2, 4), ["vaLue", "valUe"]);

st.stop();
})();
