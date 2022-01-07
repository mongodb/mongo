// Test that a pipeline of the form [{$changeStream: {}}, {$match: ...}] can rewrite the
// 'operationType' and apply it to oplog-format documents in order to filter out results as early as
// possible.
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

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

// Create a sharded collection.
const coll = createShardedCollection(st, "_id" /* shardKey */, dbName, collName, 2 /* splitAt */);

// A helper that opens a change stream with the user supplied match expression 'userMatchExpr' and
// validates that,
// 1. for each shard, the events are seen in that order as specified in 'expectedOps'
// 2. each shard returns the expected number of events
// 3. the filtering is been done at oplog level
//
// Note that invalidating events cannot be tested by this function, since these will cause the
// explain used to verify the oplog-level rewrites to fail.
function verifyNonInvalidatingOps(
    resumeAfterToken, userMatchExpr, expectedOps, expectedOplogRetDocsForEachShard) {
    const cursor =
        coll.aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr]);

    // For shard1, document id is '1' and for shard2, document id is '2'.
    const docIds = [1, 2];

    for (const op of expectedOps) {
        docIds.forEach(docId => {
            assert.soon(() => cursor.hasNext());
            const event = cursor.next();
            assert.eq(event.operationType, op, event);
            assert.eq(event.documentKey._id, docId, event);
        });
    }

    assert(!cursor.hasNext());

    // An 'executionStats' could only be captured for a non-invalidating stream.
    const stats = coll.explain("executionStats")
                      .aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr]);

    assertNumChangeStreamDocsReturnedFromShard(stats, st.rs0.name, expectedOps.length);
    assertNumChangeStreamDocsReturnedFromShard(stats, st.rs1.name, expectedOps.length);

    assertNumMatchingOplogEventsForShard(stats, st.rs0.name, expectedOplogRetDocsForEachShard);
    assertNumMatchingOplogEventsForShard(stats, st.rs1.name, expectedOplogRetDocsForEachShard);
}

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken = coll.watch([]).getResumeToken();

// These operations will create oplog events. The change stream will apply several filters on these
// series of events and ensure that the '$match' expressions are rewritten correctly.
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(coll.insert({_id: 2}));
assert.commandWorked(coll.update({_id: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.update({_id: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.replaceOne({_id: 1}, {_id: 1, foo: "baz"}));
assert.commandWorked(coll.replaceOne({_id: 2}, {_id: 2, foo: "baz"}));
assert.commandWorked(coll.deleteOne({_id: 1}));
assert.commandWorked(coll.deleteOne({_id: 2}));

// Ensure that the '$match' on the 'insert' operation type is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: "insert"}},
                         ["insert"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the 'update' operation type is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: "update"}},
                         ["update"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the 'replace' operation type is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: "replace"}},
                         ["replace"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the 'delete' operation type is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: "delete"}},
                         ["delete"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the operation type as number is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: 1}},
                         [] /* expectedOps */,
                         0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on an unknown operation type cannot be rewritten to the oplog format.
// The oplog cursor should return '4' documents.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: "unknown"}},
                         [] /* expectedOps */,
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on an empty string operation type cannot be rewritten to the oplog
// format. The oplog cursor should return '4' documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: ""}},
                         [] /* expectedOps */,
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on operation type with inequality operator cannot be rewritten to the
// oplog format. The oplog cursor should return '4' documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$gt: "insert"}}},
                         ["update", "replace"],
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on operation type sub-field can be rewritten to the oplog format. The
// oplog cursor should return '0' documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {"operationType.subField": "subOperation"}},
                         [] /* expectedOps */,
                         0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$eq: null' on operation type sub-field can be rewritten to the oplog format. The
// oplog cursor should return all documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {"operationType.subField": {$eq: null}}},
                         ["insert", "update", "replace", "delete"] /* expectedOps */,
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the operation type with '$in' is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$in: ["insert", "update"]}}},
                         ["insert", "update"],
                         2 /* expectedOplogRetDocsForEachShard */);

// Ensure that for the '$in' with one valid and one invalid operation type, rewrite to the oplog
// format will be abandoned. The oplog cursor should return '4' documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$in: ["insert", "unknown"]}}},
                         ["insert"],
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$in' on an unknown operation type cannot be rewritten. The oplog
// cursor should return '4' documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$in: ["unknown"]}}},
                         [] /* expectedOps */,
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$in' with operation type as number is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$in: [1]}}},
                         [] /* expectedOps */,
                         0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$in' with operation type as a string and a regex cannot be
// rewritten. The oplog cursor should return '4' documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$in: [/^insert$/, "update"]}}},
                         ["insert", "update"] /* expectedOps */,
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the operation type with '$nin' is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$nin: ["insert"]}}},
                         ["update", "replace", "delete"],
                         3 /* expectedOplogRetDocsForEachShard */);

// Ensure that for the '$nin' with one valid and one invalid operation type, rewrite to the oplog
// format will be abandoned. The oplog cursor should return '4' documents for each shard.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$nin: ["insert", "unknown"]}}},
                         ["update", "replace", "delete"],
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$nin' with operation type as number is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {operationType: {$nin: [1]}}},
                         ["insert", "update", "replace", "delete"],
                         4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'insert' operations is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {$expr: {$eq: ["$operationType", "insert"]}}},
                         ["insert"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'update' operations is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {$expr: {$eq: ["$operationType", "update"]}}},
                         ["update"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'replace' operations is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {$expr: {$eq: ["$operationType", "replace"]}}},
                         ["replace"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'delete' operations is rewritten correctly.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {$expr: {$eq: ["$operationType", "delete"]}}},
                         ["delete"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when comparing with 'unknown'
// operation type.
verifyNonInvalidatingOps(resumeAfterToken,
                         {$match: {$expr: {$eq: ["$operationType", "unknown"]}}},
                         [] /* expectedOps */,
                         0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when '$and' is in the expression.
verifyNonInvalidatingOps(resumeAfterToken,
                         {
                             $match: {
                                 $expr: {
                                     $and: [
                                         {$gte: [{$indexOfCP: ["$operationType", "l"]}, 0]},
                                         {$gte: [{$indexOfCP: ["$operationType", "te"]}, 0]}
                                     ]
                                 }
                             }
                         },
                         ["delete"],
                         1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when '$or' is in the expression.
verifyNonInvalidatingOps(resumeAfterToken,
                         {
                             $match: {
                                 $expr: {
                                     $or: [
                                         {$gte: [{$indexOfCP: ["$operationType", "l"]}, 0]},
                                         {$gte: [{$indexOfCP: ["$operationType", "te"]}, 0]}
                                     ]
                                 }
                             }
                         },
                         ["update", "replace", "delete"],
                         3 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when '$not' is in the expression.
verifyNonInvalidatingOps(
    resumeAfterToken,
    {$match: {$expr: {$not: {$regexMatch: {input: "$operationType", regex: /e$/}}}}},
    ["insert"],
    1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when nor ({$not: {$or: [...]}}) is
// in the expression.
verifyNonInvalidatingOps(resumeAfterToken,
                         {
                             $match: {
                                 $expr: {
                                     $not: {
                                         $or: [
                                             {$eq: ["$operationType", "insert"]},
                                             {$eq: ["$operationType", "delete"]},
                                         ]
                                     }
                                 }
                             }
                         },
                         ["update", "replace"],
                         2 /* expectedOplogRetDocsForEachShard */);

st.stop();
})();
