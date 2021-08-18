// Test that a pipeline of the form [{$changeStream: {}}, {$match: <predicate>}] with a predicate
// involving the 'fullDocument' field can push down the $match and rewrite the $match and make it
// part of the oplog cursor's filter in order to filter out results as early as possible.
// @tags: [
//   featureFlagChangeStreamsRewrite,
//   requires_fcv_51,
//   requires_pipeline_optimization,
//   requires_sharding,
//   uses_change_streams,
// ]
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const dbName = "change_stream_match_pushdown_fullDocument_rewrite";
const collName = "change_stream_match_pushdown_fullDocument_rewrite";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

// Verifies the number of change streams events returned from a particular shard.
function assertNumChangeStreamDocsReturnedFromShard(shardReplSet, comment, expectedTotalReturned) {
    let result = null;
    assert.soon(() => {
        result = shardReplSet.getPrimary()
                     .getDB(dbName)
                     .getCollection("system.profile")
                     .aggregate([
                         {$match: {op: "getmore", "originatingCommand.comment": comment}},
                         {$group: {_id: null, totalReturned: {$sum: "$nreturned"}}}
                     ])
                     .toArray();
        return result.length == 1;
    });
    assert.eq(result[0].totalReturned, expectedTotalReturned, result[0]);
}

// Helper to extract the 'executionStats' from the $cursor pipeline stage.
function getOplogExecutionStatsForShard(stats, shardName) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    assert.eq(Object.keys(stats.shards[shardName].stages[0])[0], "$cursor", stats);
    return stats.shards[shardName].stages[0].$cursor.executionStats;
}

// Create a sharded collection where the shard key is 'shard'.
assertDropAndRecreateCollection(db, collName);

const coll = db.getCollection(collName);
assert.commandWorked(coll.createIndex({shard: 1}));

st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Shard the test collection and split it into two chunks: one that contains all {shard: 0}
// documents and one that contains all {shard: 1} documents.
st.shardColl(collName,
             {shard: 1} /* shard key */,
             {shard: 1} /* split at */,
             {shard: 1} /* move the chunk containing {shard: 1} to its own shard */,
             dbName,
             true);

// Enable profiling on all nodes.
st.rs0.nodes.forEach(function(node) {
    assert(node.getDB(dbName).setProfilingLevel(2));
});
st.rs1.nodes.forEach(function(node) {
    assert(node.getDB(dbName).setProfilingLevel(2));
});

// A helper that opens a change stream with the user supplied match expression 'userMatchExpr' and
// validates that:
// (1) for each shard, the events are seen in that order as specified in 'expectedOps'; and
// (2) the number of docs returned by each shard matches what we expect as specified by
//     'expectedChangeStreamDocsReturned'; and
// (3) the number of docs returned by the oplog cursor on each shard matches what we expect as
//     specified in 'expectedOplogCursorReturnedDocs'.
function verifyOps(resumeAfterToken,
                   userMatchExpr,
                   aggregateComment,
                   expectedOps,
                   expectedChangeStreamDocsReturned,
                   expectedOplogCursorReturnedDocs) {
    const cursor = coll.aggregate(
        [
            {$changeStream: {resumeAfter: resumeAfterToken, fullDocument: "updateLookup"}},
            userMatchExpr
        ],
        {comment: aggregateComment});

    for (const [op, id, shardId] of expectedOps) {
        assert.soon(() => cursor.hasNext());
        const event = cursor.next();
        assert.eq(event.operationType, op, event);
        if (id !== undefined) {
            assert.eq(event.fullDocument._id, id, event);
        }
        if (shardId !== undefined) {
            assert.eq(event.fullDocument.shard, shardId, event);
        }
    }

    assert(!cursor.hasNext());
    assertNumChangeStreamDocsReturnedFromShard(
        st.rs0, aggregateComment, expectedChangeStreamDocsReturned[0]);
    assertNumChangeStreamDocsReturnedFromShard(
        st.rs1, aggregateComment, expectedChangeStreamDocsReturned[1]);

    // An 'executionStats' could only be captured for a non-invalidating stream.
    const stats = coll.explain("executionStats")
                      .aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr],
                                 {comment: aggregateComment});

    const execStats = [
        getOplogExecutionStatsForShard(stats, st.rs0.name),
        getOplogExecutionStatsForShard(stats, st.rs1.name)
    ];

    assert.eq(execStats[0].nReturned, expectedOplogCursorReturnedDocs[0], execStats[0]);
    assert.eq(execStats[1].nReturned, expectedOplogCursorReturnedDocs[1], execStats[1]);
}

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken = coll.watch([]).getResumeToken();

// These operations will create oplog events. The change stream will apply several filters on these
// series of events and ensure that the '$match' expressions are rewritten correctly.
assert.commandWorked(coll.insert({_id: 2, shard: 0}));
assert.commandWorked(coll.insert({_id: 3, shard: 0}));
assert.commandWorked(coll.insert({_id: 2, shard: 1}));
assert.commandWorked(coll.insert({_id: 3, shard: 1}));
assert.commandWorked(coll.replaceOne({_id: 2, shard: 0}, {_id: 2, shard: 0, foo: "a"}));
assert.commandWorked(coll.replaceOne({_id: 3, shard: 0}, {_id: 3, shard: 0, foo: "a"}));
assert.commandWorked(coll.replaceOne({_id: 2, shard: 1}, {_id: 2, shard: 1, foo: "a"}));
assert.commandWorked(coll.replaceOne({_id: 3, shard: 1}, {_id: 3, shard: 1, foo: "a"}));
assert.commandWorked(coll.update({_id: 2, shard: 0}, {$set: {foo: "b"}}));
assert.commandWorked(coll.update({_id: 3, shard: 0}, {$set: {foo: "b"}}));
assert.commandWorked(coll.update({_id: 2, shard: 1}, {$set: {foo: "b"}}));
assert.commandWorked(coll.update({_id: 3, shard: 1}, {$set: {foo: "b"}}));

// This helper will execute 'delete' operations to create oplog events. We defer executing the
// 'delete' operations so that the 'update' operations may look up the relevant document in the
// collection.
const runDeleteOps = () => {
    assert.commandWorked(coll.deleteOne({_id: 2, shard: 0}));
    assert.commandWorked(coll.deleteOne({_id: 3, shard: 0}));
    assert.commandWorked(coll.deleteOne({_id: 2, shard: 1}));
    assert.commandWorked(coll.deleteOne({_id: 3, shard: 1}));
};

// This helper takes an operation 'op' and calls verifyOps() multiple times with 'op' to exercise
// several different testcases.
const runVerifyOpsTestcases = (op) => {
    // 'delete' operations don't have a 'fullDocument' field, so we handle them as a special case.
    if (op == "delete") {
        // Test out the '{$exists: true}' predicate on the full 'fullDocument' field.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, fullDocument: {$exists: true}}},
                  "rewritten_" + op + "_with_exists_true_predicate_on_fullDocument",
                  [],
                  [0, 0] /* expectedChangeStreamDocsReturned */,
                  [0, 0] /* expectedOplogCursorReturnedDocs */);

        // Test out the '{$exists: false}' predicate on the full 'fullDocument' field.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, fullDocument: {$exists: false}}},
                  "rewritten_" + op + "_with_exists_false_predicate_on_fullDocument",
                  [[op], [op], [op], [op]],
                  [2, 2] /* expectedChangeStreamDocsReturned */,
                  [2, 2] /* expectedOplogCursorReturnedDocs */);

        return;
    }

    // Initialize 'doc' so that it matches the 'fullDocument' field of one of the events where
    // operationType == 'op'. For all 'insert' events, 'fullDocument' only has the '_id' field and
    // the 'shard' field. For 'replace' and 'update' events, 'fullDocument' also has a 'foo' field.
    const doc = {_id: 2, shard: 0};
    if (op != "insert") {
        doc.foo = (op == "replace" ? "a" : "b");
    }

    // Note: for operations of type 'update', the 'fullDocument' field is not populated until midway
    // through the pipeline. We therefore cannot rewrite predicates on 'fullDocument' into the oplog
    // for this operation type. As a result, the tests below verify that the number of documents
    // returned by the oplog scan are different for updates than for other event types.

    // Test out a predicate on the full 'fullDocument' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocument: doc}},
              "rewritten_" + op + "_with_eq_predicate_on_fullDocument",
              [[op, 2, 0]],
              [1, 0] /* expectedChangeStreamDocsReturned */,
              op != "update" ? [1, 0] : [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a predicate on 'fullDocument._id'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocument._id": {$lt: 3}}},
              "rewritten_" + op + "_with_lt_predicate_on_fullDocument_id",
              [[op, 2, 0], [op, 2, 1]],
              [1, 1] /* expectedChangeStreamDocsReturned */,
              op != "update" ? [1, 1] : [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a predicate on 'fullDocument.shard'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocument.shard": {$gt: 0}}},
              "rewritten_" + op + "_with_gt_predicate_on_fullDocument_shard",
              [[op, 2, 1], [op, 3, 1]],
              [0, 2] /* expectedChangeStreamDocsReturned */,
              op != "update" ? [0, 2] : [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a negated predicate on the full 'fullDocument' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocument: {$not: {$eq: doc}}}},
              "rewritten_" + op + "_with_negated_eq_predicate_on_fullDocument",
              [[op, 3, 0], [op, 2, 1], [op, 3, 1]],
              [1, 2] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a negated predicate on 'fullDocument._id'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocument._id": {$not: {$lt: 3}}}},
              "rewritten_" + op + "_with_negated_lt_predicate_on_fullDocument_id",
              [[op, 3, 0], [op, 3, 1]],
              [1, 1] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a negated predicate on 'fullDocument.shard'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocument.shard": {$not: {$gt: 0}}}},
              "rewritten_" + op + "_with_negated_gt_predicate_on_fullDocument_shard",
              [[op, 2, 0], [op, 3, 0]],
              [2, 0] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out the '{$exists: true}' predicate on the full 'fullDocument' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocument: {$exists: true}}},
              "rewritten_" + op + "_with_exists_true_predicate_on_fullDocument",
              [[op, 2, 0], [op, 3, 0], [op, 2, 1], [op, 3, 1]],
              [2, 2] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out the '{$exists: false}' predicate on the full 'fullDocument' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocument: {$exists: false}}},
              "rewritten_" + op + "_with_exists_false_predicate_on_fullDocument",
              [],
              [0, 0] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);
};

// Verify '$match's on the 'update' operation type with various predicates get rewritten correctly.
// We intentionally do this before executing the 'delete' operations so that post-image lookup will
// be successful.
runVerifyOpsTestcases("update");

// Now that we're done verifying 'update' events, we can execute the 'delete' operations.
runDeleteOps();

// Ensure that '$match' on 'insert', 'replace', and 'delete' operation types with various predicates
// are rewritten correctly.
runVerifyOpsTestcases("insert");
runVerifyOpsTestcases("replace");
runVerifyOpsTestcases("delete");

st.stop();
})();
