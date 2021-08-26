// Test that a pipeline of the form [{$changeStream: {}}, {$match: <predicate>}] with a predicate
// involving the 'documentKey' field can push down the $match and rewrite the $match and make it
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

const dbName = "change_stream_match_pushdown_documentKey_rewrite";
const collName = "change_stream_match_pushdown_documentKey_rewrite";

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

// Returns a newly created sharded collection, where shard key is 'shard'.
const coll = (() => {
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
    return coll;
})();

// Sets up the 'system.profile' collection for profiling.
(function setupProfiler() {
    // Create a sufficiently large 'system.profile' collection of size 5MB to avoid rolling over.
    db.createCollection("system.profile", {capped: true, size: 5 * 1000 * 1000});

    st.rs0.nodes.forEach(function(node) {
        assert(node.getDB(dbName).setProfilingLevel(2));
    });
    st.rs1.nodes.forEach(function(node) {
        assert(node.getDB(dbName).setProfilingLevel(2));
    });
})();

(function testOperationTypeRewrites() {
    // A helper that opens a change stream with the user supplied match expression 'userMatchExpr'
    // and validates that:
    // (1) for each shard, the events are seen in that order as specified in 'expectedOps'; and
    // (2) each shard returns the expected number of events; and
    // (3) the number of docs returned by the oplog cursor on each shard matches what we expect
    //     as specified in 'expectedOplogCursorReturnedDocs'.
    const verifyOps = (resumeAfterToken,
                       userMatchExpr,
                       aggregateComment,
                       expectedOps,
                       expectedOplogCursorReturnedDocs) => {
        const cursor =
            coll.aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr],
                           {comment: aggregateComment});

        let expectedChangeStreamDocsReturned = [0, 0];
        for (const [op, id, shardId] of expectedOps) {
            assert.soon(() => cursor.hasNext());
            const event = cursor.next();
            assert.eq(event.operationType, op, event);
            assert.eq(event.documentKey._id, id, event);
            assert.eq(event.documentKey.shard, shardId, event);
            if (shardId == 0 || shardId == 1) {
                ++expectedChangeStreamDocsReturned[shardId];
            }
        }

        assert(!cursor.hasNext());
        assertNumChangeStreamDocsReturnedFromShard(
            st.rs0, aggregateComment, expectedChangeStreamDocsReturned[0]);
        assertNumChangeStreamDocsReturnedFromShard(
            st.rs1, aggregateComment, expectedChangeStreamDocsReturned[1]);

        // An 'executionStats' could only be captured for a non-invalidating stream.
        const stats =
            coll.explain("executionStats")
                .aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr],
                           {comment: aggregateComment});

        const execStats = [
            getOplogExecutionStatsForShard(stats, st.rs0.name),
            getOplogExecutionStatsForShard(stats, st.rs1.name)
        ];

        assert.eq(execStats[0].nReturned, expectedOplogCursorReturnedDocs[0], execStats[0]);
        assert.eq(execStats[1].nReturned, expectedOplogCursorReturnedDocs[1], execStats[1]);
    };

    // Open a change stream and store the resume token. This resume token will be used to replay the
    // stream after this point.
    const resumeAfterToken = coll.watch([]).getResumeToken();

    // These operations will create oplog events. The change stream will apply several filters on
    // these series of events and ensure that the '$match' expressions are rewritten correctly.
    assert.commandWorked(coll.insert({_id: 2, shard: 0}));
    assert.commandWorked(coll.insert({_id: 3, shard: 0, z: 4}));
    assert.commandWorked(coll.insert({_id: 2, shard: 1, z: 4}));
    assert.commandWorked(coll.insert({_id: 3, shard: 1}));
    assert.commandWorked(coll.update({_id: 2, shard: 0}, {$set: {foo: "a"}}));
    assert.commandWorked(coll.update({_id: 3, shard: 0}, {$set: {foo: "a"}}));
    assert.commandWorked(coll.update({_id: 2, shard: 1}, {$set: {foo: "a"}}));
    assert.commandWorked(coll.update({_id: 3, shard: 1}, {$set: {foo: "a"}}));
    assert.commandWorked(coll.replaceOne({_id: 2, shard: 0}, {_id: 2, shard: 0, foo: "b"}));
    assert.commandWorked(coll.replaceOne({_id: 3, shard: 0}, {_id: 3, shard: 0, z: 4, foo: "b"}));
    assert.commandWorked(coll.replaceOne({_id: 2, shard: 1}, {_id: 2, shard: 1, z: 4, foo: "b"}));
    assert.commandWorked(coll.replaceOne({_id: 3, shard: 1}, {_id: 3, shard: 1, foo: "b"}));
    assert.commandWorked(coll.deleteOne({_id: 2, shard: 0}));
    assert.commandWorked(coll.deleteOne({_id: 3, shard: 0}));
    assert.commandWorked(coll.deleteOne({_id: 2, shard: 1}));
    assert.commandWorked(coll.deleteOne({_id: 3, shard: 1}));

    // Enable a failpoint that will prevent $expr match expressions from generating $_internalExprEq
    // or similar expressions. This ensures that the following test-cases only exercise the $expr
    // rewrites.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));

    // Ensure that the '$match' on the 'insert', 'update', 'replace', and 'delete' operation types
    // with various predicates are rewritten correctly.
    for (const op of ["insert", "update", "replace", "delete"]) {
        // Test out a predicate on the full 'documentKey' field.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, documentKey: {shard: 0, _id: 2}}},
                  "rewritten_" + op + "_with_eq_predicate_on_documentKey",
                  [[op, 2, 0]],
                  [1, 0] /* expectedOplogCursorReturnedDocs */);

        // Test out a predicate on 'documentKey._id'.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, "documentKey._id": 2}},
                  "rewritten_" + op + "_with_eq_predicate_on_documentKey_id",
                  [[op, 2, 0], [op, 2, 1]],
                  [1, 1] /* expectedOplogCursorReturnedDocs */);

        // Test out a predicate on 'documentKey.shard'.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, "documentKey.shard": 1}},
                  "rewritten_" + op + "_with_eq_predicate_on_documentKey_shard",
                  [[op, 2, 1], [op, 3, 1]],
                  [0, 2] /* expectedOplogCursorReturnedDocs */);

        // Test out a negated predicate on the full 'documentKey' field. It's not possible to
        // rewrite this predicate and make it part of the oplog filter, so we expect the oplog
        // cursor to return 2 docs on each shard.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, documentKey: {$not: {$eq: {shard: 0, _id: 2}}}}},
                  "rewritten_" + op + "_with_negated_eq_predicate_on_documentKey",
                  [[op, 3, 0], [op, 2, 1], [op, 3, 1]],
                  [2, 2] /* expectedOplogCursorReturnedDocs */);

        // Test out a negated predicate on 'documentKey._id'.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, "documentKey._id": {$not: {$eq: 2}}}},
                  "rewritten_" + op + "_with_negated_eq_predicate_on_documentKey_id",
                  [[op, 3, 0], [op, 3, 1]],
                  [1, 1] /* expectedOplogCursorReturnedDocs */);

        // Test out a negated predicate on 'documentKey.shard'. It's not possible to rewrite this
        // predicate and make it part of the oplog filter, so we expect the oplog cursor to return 2
        // docs on each shard.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, "documentKey.shard": {$not: {$eq: 1}}}},
                  "rewritten_" + op + "_with_negated_eq_predicate_on_documentKey_shard",
                  [[op, 2, 0], [op, 3, 0]],
                  [2, 2] /* expectedOplogCursorReturnedDocs */);

        // Test out the '{$exists: false}' predicate on a field that doesn't exist in 'documentKey'
        // but that does exist in some of the underlying documents.
        verifyOps(resumeAfterToken,
                  {$match: {operationType: op, "documentKey.z": {$exists: false}}},
                  "rewritten_" + op + "_with_exists_false_predicate_on_documentKey_z",
                  [[op, 2, 0], [op, 3, 0], [op, 2, 1], [op, 3, 1]],
                  [2, 2] /* expectedOplogCursorReturnedDocs */);

        // Test out an $expr predicate on the full 'documentKey' field.
        verifyOps(
            resumeAfterToken,
            {
                $match: {
                    $and:
                        [{operationType: op}, {$expr: {$eq: ["$documentKey", {shard: 0, _id: 2}]}}]
                }
            },
            "rewritten_" + op + "_with_expr_eq_predicate_on_documentKey",
            [[op, 2, 0]],
            [2, 2] /* expectedOplogCursorReturnedDocs */);

        // Test out a negated predicate on the full 'documentKey' field.
        verifyOps(resumeAfterToken,
                  {
                      $match: {
                          $and: [
                              {operationType: op},
                              {$expr: {$not: {$eq: ["$documentKey", {shard: 0, _id: 2}]}}}
                          ]
                      }
                  },
                  "rewritten_" + op + "_with_negated_expr_eq_predicate_on_documentKey",
                  [[op, 3, 0], [op, 2, 1], [op, 3, 1]],
                  [2, 2] /* expectedOplogCursorReturnedDocs */);

        // Test out an $expr predicate on 'documentKey._id'.
        verifyOps(resumeAfterToken,
                  {$match: {$and: [{operationType: op}, {$expr: {$eq: ["$documentKey._id", 2]}}]}},
                  "rewritten_" + op + "_with_expr_eq_predicate_on_documentKey_id",
                  [[op, 2, 0], [op, 2, 1]],
                  [1, 1] /* expectedOplogCursorReturnedDocs */);

        // Test out a negated $expr predicate on 'documentKey._id'.
        verifyOps(
            resumeAfterToken,
            {
                $match:
                    {$and: [{operationType: op}, {$expr: {$not: {$eq: ["$documentKey._id", 2]}}}]}
            },
            "rewritten_" + op + "_with_negated_expr_eq_predicate_on_documentKey_id",
            [[op, 3, 0], [op, 3, 1]],
            [1, 1] /* expectedOplogCursorReturnedDocs */);

        // Test out an $expr predicate on 'documentKey.shard'.
        verifyOps(
            resumeAfterToken,
            {$match: {$and: [{operationType: op}, {$expr: {$eq: ["$documentKey.shard", 1]}}]}},
            "rewritten_" + op + "_with_expr_eq_predicate_on_documentKey_shard",
            [[op, 2, 1], [op, 3, 1]],
            [2, 2] /* expectedOplogCursorReturnedDocs */);

        // Test out a negated $expr predicate on 'documentKey.shard'.
        verifyOps(
            resumeAfterToken,
            {
                $match:
                    {$and: [{operationType: op}, {$expr: {$not: {$eq: ["$documentKey.shard", 1]}}}]}
            },
            "rewritten_" + op + "_with_negated_expr_eq_predicate_on_documentKey_shard",
            [[op, 2, 0], [op, 3, 0]],
            [2, 2] /* expectedOplogCursorReturnedDocs */);
    }
})();

st.stop();
})();
