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

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const dbName = "change_stream_match_pushdown_and_rewrite";
const collName = "change_stream_match_pushdown_and_rewrite";
const collNameAlternate = "change_stream_match_pushdown_and_rewrite_alternate";

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

// Returns a newly created sharded collection, where shard key is '_id'.
const coll = (() => {
    assertDropAndRecreateCollection(db, collName);

    const coll = db.getCollection(collName);
    assert.commandWorked(coll.createIndex({_id: 1}));

    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    // Shard the test collection and split it into two chunks: one that contains all {_id: 1}
    // documents and one that contains all {_id: 2} documents.
    st.shardColl(collName,
                 {_id: 1} /* shard key */,
                 {_id: 2} /* split at */,
                 {_id: 2} /* move the chunk containing {_id: 2} to its own shard */,
                 dbName,
                 true);
    return coll;
})();

// Create a second (unsharded) test collection for validating transactions that insert into multiple
// collections.
assert.commandWorked(db.createCollection(collNameAlternate));

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

(function testBasicMatchPushdownAndTransactionRewrites() {
    const changeStreamComment = "change_stream_match_pushdown_and_rewrite change stream";
    const changeStream = coll.aggregate([{$changeStream: {}}, {$match: {operationType: "insert"}}],
                                        {comment: changeStreamComment});

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

    // Verify the number of documents seen from each shard by the mongoS pipeline. Because we expect
    // the $match to be pushed down to the shards, we expect to only see the 1 "insert" operation on
    // each shard. All other operations should be filtered out on the shards.
    assertNumChangeStreamDocsReturnedFromShard(st.rs0, changeStreamComment, 1);
    assertNumChangeStreamDocsReturnedFromShard(st.rs1, changeStreamComment, 1);

    // Run the same change stream again, this time with "executionStats" to get more detailed
    // information about how many documents are processed by each stage. Note that 'event1' will not
    // be included in the results set this time, because we are using it as the resume point, but
    // will still be returned from the shard and get processed on the mongoS.
    const stats = coll.explain("executionStats").aggregate([
        {$changeStream: {resumeAfter: event1._id}},
        {$match: {operationType: "insert"}}
    ]);

    // Because it is possible to rewrite the {operationType: "insert"} predicate so that it applies
    // to the oplog entry, we expect the $match to get pushed all the way to the initial oplog
    // query. This query executes in an internal "$cursor" stage, and we expect to see exactly 1
    // document from this stage on each shard.
    const execStatsShard0 = getOplogExecutionStatsForShard(stats, st.rs0.name);
    assert.eq(execStatsShard0.nReturned, 1, execStatsShard0);

    const execStatsShard1 = getOplogExecutionStatsForShard(stats, st.rs1.name);
    assert.eq(execStatsShard1.nReturned, 1, execStatsShard1);

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
    // change stream left off. This change stream will only observe the 6 operations that occur in
    // the transaction and will filter out everything except the 2 inserts.
    const txnChangeStreamComment =
        "change_stream_match_pushdown_and_rewrite change stream for transaction";
    const txnChangeStream = coll.aggregate(
        [{$changeStream: {resumeAfter: event2._id}}, {$match: {operationType: "insert"}}],
        {comment: txnChangeStreamComment});

    // Verify correct operation of the change stream.
    assert.soon(() => txnChangeStream.hasNext());
    const event3 = txnChangeStream.next();
    assert.eq(event3.operationType, "insert", event3);
    assert.eq(event3.documentKey._id, 1, event3);

    assert.soon(() => txnChangeStream.hasNext());
    const event4 = txnChangeStream.next();
    assert.eq(event4.operationType, "insert", event4);
    assert.eq(event4.documentKey._id, 2, event4);

    assert(!txnChangeStream.hasNext());
    txnChangeStream.close();

    // Verify the number of documents seen from each shard by the mongoS pipeline. As before, we
    // expect that everything except the inserts will be filtered on the shard, limiting the number
    // of events the mongoS needs to retrieve.
    assertNumChangeStreamDocsReturnedFromShard(st.rs0, txnChangeStreamComment, 1);

    // Note that the event we are resuming from is sent to the mongoS from shard 2, even though it
    // gets filtered out, which is why we see 2 events here.
    assertNumChangeStreamDocsReturnedFromShard(st.rs1, txnChangeStreamComment, 2);

    // Generate a second transaction.
    session.startTransaction({readConcern: {level: "majority"}});

    assert.commandWorked(sessionColl.insert({_id: 1, a: 1, string: "vaLue"}));
    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {foo: "bar"}}));
    assert.commandWorked(sessionColl.remove({_id: 1}));
    assert.commandWorked(sessionColl.insert({_id: 2, a: 2, string: "valUe"}));
    assert.commandWorked(sessionColl.update({_id: 2}, {$set: {foo: "bar"}}));
    assert.commandWorked(sessionColl.remove({_id: 2}));

    assert.commandWorked(session.commitTransaction_forTesting());

    // This change stream targets transactions from this session but filters out the first
    // transaction.
    const txnStats = coll.explain("executionStats").aggregate([
        {$changeStream: {resumeAfter: event1._id}},
        {$match: {operationType: "insert", lsid: event3.lsid, txnNumber: {$ne: event3.txnNumber}}}
    ]);

    // The "lsid" and "txnNumber" filters should get pushed all the way to the initial oplog query
    // in the $cursor stage, meaning that every oplog entry gets filtered out except the
    // 'commitTransaction' on each shard for the one transaction we select with our filter.
    const txnExecStatsShard0 = getOplogExecutionStatsForShard(txnStats, st.rs0.name);
    assert.eq(txnExecStatsShard0.nReturned, 1, txnExecStatsShard0);

    const txnExecStatsShard1 = getOplogExecutionStatsForShard(txnStats, st.rs1.name);
    assert.eq(txnExecStatsShard1.nReturned, 1, txnExecStatsShard1);

    // Ensure that optimization does not attempt to create a filter that disregards the collation.
    const collationChangeStream = coll.aggregate(
        [
            {$changeStream: {resumeAfter: resumeAfterToken}},
            {$match: {"fullDocument.string": "value"}}
        ],
        {collation: {locale: "en_US", strength: 2}});

    ["Value", "vAlue", "vaLue", "valUe"].forEach(val => {
        assert.soon(() => collationChangeStream.hasNext());
        const fullDocumentEvent = collationChangeStream.next();
        assert.eq(fullDocumentEvent.fullDocument.string, val, fullDocumentEvent);
    });

    assert(!collationChangeStream.hasNext());
    collationChangeStream.close();
})();

(function testOperationTypeRewrites() {
    // A helper that opens a change stream with the user supplied match expression 'userMatchExpr'
    // and validates that,
    // 1. for each shard, the events are seen in that order as specified in 'expectedOps'
    // 2. each shard returns the expected number of events
    // 3. the filtering is been done at oplog level
    //
    // Note that invalidating events cannot be tested by this function, since these will cause the
    // explain used to verify the oplog-level rewrites to fail.
    const verifyNonInvalidatingOps = (resumeAfterToken,
                                      userMatchExpr,
                                      aggregateComment,
                                      expectedOps,
                                      expectedOplogCursorReturnedDocs) => {
        const cursor =
            coll.aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr],
                           {comment: aggregateComment});

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
        assertNumChangeStreamDocsReturnedFromShard(st.rs0, aggregateComment, expectedOps.length);
        assertNumChangeStreamDocsReturnedFromShard(st.rs1, aggregateComment, expectedOps.length);

        // An 'executionStats' could only be captured for a non-invalidating stream.
        const stats =
            coll.explain("executionStats")
                .aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr],
                           {comment: aggregateComment});

        const execStatsShard0 = getOplogExecutionStatsForShard(stats, st.rs0.name);
        assert.eq(execStatsShard0.nReturned, expectedOplogCursorReturnedDocs, execStatsShard0);

        const execStatsShard1 = getOplogExecutionStatsForShard(stats, st.rs1.name);
        assert.eq(execStatsShard1.nReturned, expectedOplogCursorReturnedDocs, execStatsShard1);
    };

    // Open a change stream and store the resume token. This resume token will be used to replay the
    // stream after this point.
    const resumeAfterToken = coll.watch([]).getResumeToken();

    // These operations will create oplog events. The change stream will apply several filters on
    // these series of events and ensure that the '$match' expressions are rewritten correctly.
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
                             "change_stream_rewritten_insert_op_type",
                             ["insert"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on the 'update' operation type is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "update"}},
                             "change_stream_rewritten_update_op_type",
                             ["update"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on the 'replace' operation type is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "replace"}},
                             "change_stream_rewritten_replace_op_type",
                             ["replace"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on the 'delete' operation type is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "delete"}},
                             "change_stream_rewritten_delete_op_type",
                             ["delete"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on the operation type as number is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: 1}},
                             "change_stream_rewritten_invalid_number_op_type",
                             [] /* expectedOps */,
                             0 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on an unknown operation type cannot be rewritten to the oplog
    // format. The oplog cursor should return '4' documents.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "unknown"}},
                             "change_stream_rewritten_unknown_op_type",
                             [] /* expectedOps */,
                             4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on an empty string operation type cannot be rewritten to the oplog
    // format. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: ""}},
                             "change_stream_rewritten_empty_op_type",
                             [] /* expectedOps */,
                             4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on operation type with inequality operator cannot be rewritten to
    // the oplog format. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$gt: "insert"}}},
                             "change_stream_rewritten_inequality_op_type",
                             ["update", "replace"],
                             4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on operation type sub-field can be rewritten to
    // the oplog format. The oplog cursor should return '0' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {"operationType.subField": "subOperation"}},
                             "change_stream_rewritten_sub_op_type",
                             [] /* expectedOps */,
                             0 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on the operation type with '$in' is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$in: ["insert", "update"]}}},
                             "change_stream_rewritten_op_type_with_in_expr",
                             ["insert", "update"],
                             2 /* expectedOplogCursorReturnedDocs */);

    // Ensure that for the '$in' with one valid and one invalid operation type, rewrite to the
    // oplog format will be abandoned. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {operationType: {$in: ["insert", "unknown"]}}},
        "change_stream_rewritten_op_type_with_in_expr_with_one_invalid_op_type",
        ["insert"],
        4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' with '$in' on an unknown operation type cannot be rewritten. The
    // oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$in: ["unknown"]}}},
                             "change_stream_rewritten_op_type_with_in_expr_with_unknown_op_type",
                             [] /* expectedOps */,
                             4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' with '$in' with operation type as number is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$in: [1]}}},
                             "change_stream_rewritten_op_type_with_in_expr_with_number_op_type",
                             [] /* expectedOps */,
                             0 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' with '$in' with operation type as a string and a regex cannot be
    // rewritten. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {operationType: {$in: [/^insert$/, "update"]}}},
        "change_stream_rewritten_op_type_with_in_expr_with_string_and_regex_op_type",
        ["insert", "update"] /* expectedOps */,
        4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' on the operation type with '$nin' is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$nin: ["insert"]}}},
                             "change_stream_rewritten_op_type_with_nin_expr",
                             ["update", "replace", "delete"],
                             3 /* expectedOplogCursorReturnedDocs */);

    // Ensure that for the '$nin' with one valid and one invalid operation type, rewrite to the
    // oplog format will be abandoned. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {operationType: {$nin: ["insert", "unknown"]}}},
        "change_stream_rewritten_op_type_with_nin_expr_with_one_invalid_op_type",
        ["update", "replace", "delete"],
        4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' with '$nin' with operation type as number is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$nin: [1]}}},
                             "change_stream_rewritten_op_type_with_nin_expr_with_number_op_type",
                             ["insert", "update", "replace", "delete"],
                             4 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' to match only 'insert' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "insert"]}}},
                             "change_stream_rewritten_op_type_eq_insert_in_expr",
                             ["insert"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' to match only 'update' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "update"]}}},
                             "change_stream_rewritten_op_type_eq_update_in_expr",
                             ["update"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' to match only 'replace' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "replace"]}}},
                             "change_stream_rewritten_op_type_eq_replace_in_expr",
                             ["replace"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' to match only 'delete' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "delete"]}}},
                             "change_stream_rewritten_op_type_eq_delete_in_expr",
                             ["delete"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' is rewritten correctly when comparing with 'unknown'
    // operation type.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "unknown"]}}},
                             "change_stream_rewritten_op_type_eq_unknown_in_expr",
                             [] /* expectedOps */,
                             0 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' is rewritten correctly when '$and' is in the
    // expression.
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
                             "change_stream_rewritten_op_type_in_expr_with_and",
                             ["delete"],
                             1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' is rewritten correctly when '$or' is in the
    // expression.
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
                             "change_stream_rewritten_op_type_in_expr_with_or",
                             ["update", "replace", "delete"],
                             3 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' is rewritten correctly when '$not' is in the
    // expression.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {$expr: {$not: {$regexMatch: {input: "$operationType", regex: /e$/}}}}},
        "change_stream_rewritten_op_type_in_expr_with_not",
        ["insert"],
        1 /* expectedOplogCursorReturnedDocs */);

    // Ensure that the '$match' using '$expr' is rewritten correctly when nor ({$not: {$or: [...]}})
    // is in the expression.
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
                             "change_stream_rewritten_op_type_in_expr_with_nor",
                             ["update", "replace"],
                             2 /* expectedOplogCursorReturnedDocs */);
})();

st.stop();
})();
