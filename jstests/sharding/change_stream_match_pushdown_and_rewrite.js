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
const collName = "coll1";
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
function createShardedCollection(collName, splitAt) {
    assertDropAndRecreateCollection(db, collName);

    const coll = db.getCollection(collName);
    assert.commandWorked(coll.createIndex({_id: 1}));

    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    // Shard the test collection and split it into two chunks: one that contains all {_id: <lt
    // splitAt>} documents and one that contains all {_id: <gte splitAt>} documents.
    st.shardColl(collName,
                 {_id: 1} /* shard key */,
                 {_id: splitAt} /* split at */,
                 {_id: splitAt} /* move the chunk containing {_id: splitAt} to its own shard */,
                 dbName,
                 true);
    return coll;
}

// Create a sharded collection.
const coll = createShardedCollection(collName, 2 /* splitAt */);

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
                                      expectedOplogRetDocsForEachShard) => {
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
        assert.eq(execStatsShard0.nReturned, expectedOplogRetDocsForEachShard, execStatsShard0);

        const execStatsShard1 = getOplogExecutionStatsForShard(stats, st.rs1.name);
        assert.eq(execStatsShard1.nReturned, expectedOplogRetDocsForEachShard, execStatsShard1);
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
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the 'update' operation type is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "update"}},
                             "change_stream_rewritten_update_op_type",
                             ["update"],
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the 'replace' operation type is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "replace"}},
                             "change_stream_rewritten_replace_op_type",
                             ["replace"],
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the 'delete' operation type is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "delete"}},
                             "change_stream_rewritten_delete_op_type",
                             ["delete"],
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the operation type as number is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: 1}},
                             "change_stream_rewritten_invalid_number_op_type",
                             [] /* expectedOps */,
                             0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on an unknown operation type cannot be rewritten to the oplog
    // format. The oplog cursor should return '4' documents.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: "unknown"}},
                             "change_stream_rewritten_unknown_op_type",
                             [] /* expectedOps */,
                             4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on an empty string operation type cannot be rewritten to the oplog
    // format. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: ""}},
                             "change_stream_rewritten_empty_op_type",
                             [] /* expectedOps */,
                             4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on operation type with inequality operator cannot be rewritten to
    // the oplog format. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$gt: "insert"}}},
                             "change_stream_rewritten_inequality_op_type",
                             ["update", "replace"],
                             4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on operation type sub-field can be rewritten to
    // the oplog format. The oplog cursor should return '0' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {"operationType.subField": "subOperation"}},
                             "change_stream_rewritten_sub_op_type",
                             [] /* expectedOps */,
                             0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the operation type with '$in' is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$in: ["insert", "update"]}}},
                             "change_stream_rewritten_op_type_with_in_expr",
                             ["insert", "update"],
                             2 /* expectedOplogRetDocsForEachShard */);

    // Ensure that for the '$in' with one valid and one invalid operation type, rewrite to the
    // oplog format will be abandoned. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {operationType: {$in: ["insert", "unknown"]}}},
        "change_stream_rewritten_op_type_with_in_expr_with_one_invalid_op_type",
        ["insert"],
        4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' with '$in' on an unknown operation type cannot be rewritten. The
    // oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$in: ["unknown"]}}},
                             "change_stream_rewritten_op_type_with_in_expr_with_unknown_op_type",
                             [] /* expectedOps */,
                             4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' with '$in' with operation type as number is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$in: [1]}}},
                             "change_stream_rewritten_op_type_with_in_expr_with_number_op_type",
                             [] /* expectedOps */,
                             0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' with '$in' with operation type as a string and a regex cannot be
    // rewritten. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {operationType: {$in: [/^insert$/, "update"]}}},
        "change_stream_rewritten_op_type_with_in_expr_with_string_and_regex_op_type",
        ["insert", "update"] /* expectedOps */,
        4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the operation type with '$nin' is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$nin: ["insert"]}}},
                             "change_stream_rewritten_op_type_with_nin_expr",
                             ["update", "replace", "delete"],
                             3 /* expectedOplogRetDocsForEachShard */);

    // Ensure that for the '$nin' with one valid and one invalid operation type, rewrite to the
    // oplog format will be abandoned. The oplog cursor should return '4' documents for each shard.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {operationType: {$nin: ["insert", "unknown"]}}},
        "change_stream_rewritten_op_type_with_nin_expr_with_one_invalid_op_type",
        ["update", "replace", "delete"],
        4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' with '$nin' with operation type as number is rewritten correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {operationType: {$nin: [1]}}},
                             "change_stream_rewritten_op_type_with_nin_expr_with_number_op_type",
                             ["insert", "update", "replace", "delete"],
                             4 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' using '$expr' to match only 'insert' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "insert"]}}},
                             "change_stream_rewritten_op_type_eq_insert_in_expr",
                             ["insert"],
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' using '$expr' to match only 'update' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "update"]}}},
                             "change_stream_rewritten_op_type_eq_update_in_expr",
                             ["update"],
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' using '$expr' to match only 'replace' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "replace"]}}},
                             "change_stream_rewritten_op_type_eq_replace_in_expr",
                             ["replace"],
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' using '$expr' to match only 'delete' operations is rewritten
    // correctly.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "delete"]}}},
                             "change_stream_rewritten_op_type_eq_delete_in_expr",
                             ["delete"],
                             1 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' using '$expr' is rewritten correctly when comparing with 'unknown'
    // operation type.
    verifyNonInvalidatingOps(resumeAfterToken,
                             {$match: {$expr: {$eq: ["$operationType", "unknown"]}}},
                             "change_stream_rewritten_op_type_eq_unknown_in_expr",
                             [] /* expectedOps */,
                             0 /* expectedOplogRetDocsForEachShard */);

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
                             1 /* expectedOplogRetDocsForEachShard */);

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
                             3 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' using '$expr' is rewritten correctly when '$not' is in the
    // expression.
    verifyNonInvalidatingOps(
        resumeAfterToken,
        {$match: {$expr: {$not: {$regexMatch: {input: "$operationType", regex: /e$/}}}}},
        "change_stream_rewritten_op_type_in_expr_with_not",
        ["insert"],
        1 /* expectedOplogRetDocsForEachShard */);

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
                             2 /* expectedOplogRetDocsForEachShard */);
})();

(function testNamespaceRewrites() {
    // A helper that opens a change stream on the whole cluster with the user supplied match
    // expression 'userMatchExpr' and validates that:
    // 1. for each shard, the events are seen in that order as specified in 'expectedResult'
    // 2. the filtering is been done at oplog level
    const verifyOnWholeCluster = (resumeAfterToken,
                                  userMatchExpr,
                                  expectedResult,
                                  expectedOplogRetDocsForEachShard) => {
        const cursor = db.getSiblingDB("admin").aggregate([
            {$changeStream: {resumeAfter: resumeAfterToken, allChangesForCluster: true}},
            userMatchExpr
        ]);

        for (const [collOrDb, opDict] of Object.entries(expectedResult)) {
            for (const [op, eventIdentifierList] of Object.entries(opDict)) {
                eventIdentifierList.forEach(eventIdentifier => {
                    assert.soon(() => cursor.hasNext());
                    const event = cursor.next();
                    assert.eq(event.operationType, op, event);

                    if (op == "dropDatabase") {
                        assert.eq(event.ns.db, eventIdentifier, event);
                    } else if (op == "insert") {
                        assert.eq(event.documentKey._id, eventIdentifier, event);
                    } else if (op == "rename") {
                        assert.eq(event.to.coll, eventIdentifier, event);
                    } else if (op == "drop") {
                        assert.eq(event.ns.coll, eventIdentifier);
                    } else {
                        assert(false, event);
                    }

                    if (op != "dropDatabase") {
                        assert.eq(event.ns.coll, collOrDb);
                    }
                });
            }
        }

        assert(!cursor.hasNext());

        const stats = db.getSiblingDB("admin").runCommand({
            explain: {
                aggregate: 1,
                pipeline: [
                    {$changeStream: {resumeAfter: resumeAfterToken, allChangesForCluster: true}},
                    userMatchExpr
                ],
                cursor: {batchSize: 0}
            },
            verbosity: "executionStats"
        });

        const execStatsShard0 = getOplogExecutionStatsForShard(stats, st.rs0.name);
        assert.eq(execStatsShard0.nReturned, expectedOplogRetDocsForEachShard, execStatsShard0);

        const execStatsShard1 = getOplogExecutionStatsForShard(stats, st.rs1.name);
        assert.eq(execStatsShard1.nReturned, expectedOplogRetDocsForEachShard, execStatsShard1);
    };

    // Create some new collections to ensure that test cases has sufficient namespaces to verify
    // that the namespace filtering is working correctly.
    const coll2 = createShardedCollection("coll2", 4 /* splitAt */);
    const coll3 = createShardedCollection("coll.coll3", 6 /* splitAt */);
    const coll4 = createShardedCollection("coll4", 10 /* splitAt */);

    // Open a change stream and store the resume token. This resume token will be used to replay the
    // stream after this point.
    const resumeAfterToken =
        db.getSiblingDB("admin").watch([], {allChangesForCluster: true}).getResumeToken();

    // For each collection, insert 2 documents, one on each shard. These will create oplog events
    // and change stream will apply various namespace filtering on these collections to verify that
    // the namespace is rewritten correctly. Each documents also contains field names matching with
    // that of '$cmd' operations, ie. 'renameCollection', 'drop' and 'dropDatabase', but with
    // value-type other than strings. The 'ns' match filters should gracefully handle the type
    // mismatch and not throw any error.
    // Each of these inserted documents will be represented in this form in the oplog:
    // {... "o": {"_id": <id>, "renameCollection": true, "drop": {}, "dropDatabase": null}, ...}
    // A few collections are renamed and dropped to verify that these are filtered properly.
    assert.commandWorked(
        coll.insert({_id: 1, renameCollection: true, drop: {}, dropDatabase: null}));
    assert.commandWorked(
        coll.insert({_id: 2, renameCollection: true, drop: {}, dropDatabase: null}));
    assert.commandWorked(
        coll2.insert({_id: 3, renameCollection: true, drop: {}, dropDatabase: null}));
    assert.commandWorked(
        coll2.insert({_id: 4, renameCollection: true, drop: {}, dropDatabase: null}));
    assert.commandWorked(coll2.renameCollection("newColl2"));
    assert.commandWorked(
        coll3.insert({_id: 5, renameCollection: true, drop: {}, dropDatabase: null}));
    assert.commandWorked(
        coll3.insert({_id: 6, renameCollection: true, drop: {}, dropDatabase: null}));
    assert(coll3.drop());

    // Insert some documents into 'coll4' with field names which match known command types. Despite
    // the fact that these documents could potentially match with the partial 'ns' filter we rewrite
    // into the oplog, the {op: "c"} predicate we add into the filter should ensure that they are
    // correctly discarded.
    assert.commandWorked(coll4.insert(
        {_id: 7, renameCollection: coll2.getName(), drop: coll3.getName(), dropDatabase: 1}));
    assert.commandWorked(
        coll4.insert({_id: 8, renameCollection: true, drop: {}, dropDatabase: null}));
    assert.commandWorked(
        coll4.insert({_id: 9, renameCollection: "no_dot_ns", drop: "", dropDatabase: ""}));
    assert.commandWorked(coll4.insert(
        {_id: 10, renameCollection: coll2.getName(), drop: coll3.getName(), dropDatabase: 1}));
    assert.commandWorked(
        coll4.insert({_id: 11, renameCollection: true, drop: {}, dropDatabase: null}));
    assert.commandWorked(
        coll4.insert({_id: 12, renameCollection: "no_dot_ns", drop: "", dropDatabase: ""}));

    // This group of tests ensures that the '$match' on a particular namespace object only sees its
    // documents and only required document(s) are returned at the oplog for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {ns: {db: dbName, coll: "coll1"}}},
                         {coll1: {insert: [1, 2]}},
                         1 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {ns: {db: dbName, coll: "coll2"}}},
                         {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                         2 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {ns: {db: dbName, coll: "coll.coll3"}}},
                         {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                         2 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the namespace with only db component should not emit any
    // document and the oplog should not return any documents.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {ns: {db: dbName}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the namespace object with 'unknown' collection does not exists and the oplog
    // cursor returns 0 document.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {ns: {db: dbName, coll: "unknown"}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the namespace object with flipped fields does not match with the namespace object
    // and the oplog cursor returns 0 document.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {ns: {coll: "coll1", db: dbName}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the namespace object with extra fields does not match with the namespace object
    // and the oplog cursor returns 0 document.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {ns: {db: dbName, coll: "coll1", extra: "extra"}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the empty namespace object does not match with the namespace object and the oplog
    // cursor returns 0 document.
    verifyOnWholeCluster(resumeAfterToken, {$match: {ns: {}}}, {}, 0);

    // Ensure the '$match' on namespace's db should return documents for all collection and oplog
    // should return all documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": dbName}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // These cases ensure that the '$match' on regex of namespace' db, should return documents for
    // all collection and oplog should return all documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": /^change_stream_match_pushdown.*$/}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": /^(change_stream_match_pushdown.*$)/}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": /^(Change_Stream_MATCH_PUSHDOWN.*$)/i}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": /(^unknown$|^change_stream_match_pushdown.*$)/}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": /^unknown$|^change_stream_match_pushdown.*$/}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on non-existing db should not return any document and oplog should
    // not return any document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": "unknown"}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on empty db should not return any document and oplog should not
    // return any document for each shard.
    verifyOnWholeCluster(
        resumeAfterToken, {$match: {"ns.db": ""}}, {}, 0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on sub field of db should not return any document and oplog should
    // not return any document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db.extra": dbName}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // This group of tests ensures that the '$match' on collection field path should emit only the
    // required documents and oplog should return only required document(s) for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": "coll1"}},
                         {coll1: {insert: [1, 2]}},
                         1 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": "coll2"}},
                         {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                         2 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": "coll.coll3"}},
                         {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                         2 /* expectedOplogRetDocsForEachShard */);

    // This group of tests ensures that the '$match' on the regex of the collection field path
    // should emit only the required documents and oplog should return only required document(s) for
    // each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": /^col.*1/}},
                         {coll1: {insert: [1, 2]}},
                         1 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": /^col.*2/}},
                         {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                         2 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": /^col.*3/}},
                         {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                         2 /* expectedOplogRetDocsForEachShard */);

    // This group of tests ensures that the '$match' on the regex matching all collections should
    // return documents from all collection and oplog should return all document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": /^col.*/}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": /^CoLL.*/i}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the regex matching 3 collection should return documents from
    // these collections and oplog should return required documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": /^col.*1$|^col.*2$|^col.*3$/}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}
                         },
                         5 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on the regex to exclude 'coll1', 'coll2' and 'coll4' should return
    // only documents from 'coll.coll3' and oplog should return required documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": /^coll[^124]/}},
                         {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                         2 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on non-existing collection should not return any document and oplog
    // should not return any document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": "unknown"}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on empty collection should not return any document and oplog should
    // not return any document for each shard.
    verifyOnWholeCluster(
        resumeAfterToken, {$match: {"ns.coll": ""}}, {}, 0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that the '$match' on sub field of collection should not return any document and oplog
    // should not return any document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll.extra": "coll1"}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' on db should return all documents and oplog should return all documents for
    // each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$in: [dbName]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // This group of tests ensures that '$in' on regex matching the db name should return all
    // documents and oplog should return all documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$in: [/^change_stream_match.*$/]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$in: [/^change_stream_MATCH.*$/i]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that an empty '$in' on db path should not match any collection and oplog should not
    // return any document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$in: []}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' with invalid db cannot be rewritten and oplog should return all documents
    // for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$in: [dbName, 1]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' on db path with mix of string and regex can be rewritten and oplog should
    // return '0' document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$in: ["unknown1", /^unknown2$/]}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' on multiple collections should return the required documents and oplog
    // should return required documents for each shard.
    verifyOnWholeCluster(
        resumeAfterToken,
        {$match: {"ns": {$in: [{db: dbName, coll: "coll1"}, {db: dbName, coll: "coll2"}]}}},
        {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
        3 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(
        resumeAfterToken,
        {$match: {"ns.coll": {$in: ["coll1", "coll2"]}}},
        {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
        3 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' on regex of multiple collections should return the required documents and
    // oplog should return required documents for each shard.
    verifyOnWholeCluster(
        resumeAfterToken,
        {$match: {"ns.coll": {$in: [/^coll1$/, /^coll2$/]}}},
        {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
        3 /* expectedOplogRetDocsForEachShard */);

    // This group of tests ensures that '$in' on regex of matching all collections should return all
    // documents and oplog should return all documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$in: [/^coll.*$/]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$in: [/^COLL.*$/i]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that an empty '$in' should not match any collection and oplog should not return any
    // document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$in: []}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' with invalid collection cannot be rewritten and oplog should return all
    // documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$in: ["coll1", 1]}}},
                         {coll1: {insert: [1, 2]}},
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' with mix of string and regex matching collections can be rewritten and
    // oplog should return required documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$in: ["coll1", /^coll.*3$/]}}},
                         {
                             coll1: {insert: [1, 2]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         },
                         3 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$in' with mix of string and regex can be rewritten and oplog should
    // return '0' document for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$in: ["unknown1", /^unknown2$/]}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // This group of tests ensure that '$nin' on db path should return all documents and oplog
    // should return all documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$nin: []}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$nin: ["unknown"]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$nin: [/^unknown$/]}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // These group of tests ensure that '$nin' on matching db name should not return any documents
    // and oplog should return '0' documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$nin: [dbName]}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.db": {$nin: [/change_stream_match_pushdown_and_rewr.*/]}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$nin' on multiple collections should return the required documents and oplog
    // should return required documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$nin: ["coll1", "coll2", "coll4"]}}},
                         {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                         2 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$nin' on regex of multiple collections should return the required documents and
    // oplog should return required documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$nin: [/^coll1$/, /^coll2$/, /^coll4$/]}}},
                         {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                         2 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$nin' on regex of matching all collections should not return any document and
    // oplog should return '0' documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$nin: [/^coll.*$/, /^sys.*$/]}}},
                         {},
                         0 /* expectedOplogRetDocsForEachShard */);

    // Ensure that an empty '$nin' should match all collections and oplog should return all
    // documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$nin: []}}},
                         {
                             coll1: {insert: [1, 2]},
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$nin' with invalid collection cannot be rewritten and oplog should
    // return all documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$nin: ["coll1", 1]}}},
                         {
                             coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                             "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                             coll4: {insert: [7, 8, 9, 10, 11, 12]}
                         },
                         8 /* expectedOplogRetDocsForEachShard */);

    // Ensure that '$nin' with mix of string and regex can be rewritten and oplog should
    // return required documents for each shard.
    verifyOnWholeCluster(resumeAfterToken,
                         {$match: {"ns.coll": {$nin: ["coll1", /^coll2$/, "coll4"]}}},
                         {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                         2 /* expectedOplogRetDocsForEachShard */);

    // At this stage, the coll2 has been renamed to 'newColl2' and coll3 has been dropped. The test
    // from here will drop the database and ensure that the 'ns' filter when applied over the
    // collection should only emit the 'drop' event for that collection and not the 'dropDatabase'
    // event. It should be noted that for 'newColl2' and 'coll3', the 'dropDatabase' will be no-op
    // and will not emit any documents.

    // Open a new change streams and verify that from here onwards the events related to
    // 'dropDatabase' are seen.
    const secondResumeAfterToken =
        db.getSiblingDB("admin").watch([], {allChangesForCluster: true}).getResumeToken();

    assert.commandWorked(db.dropDatabase());

    // This group of tests ensures that the match on 'coll1' only sees the 'drop' events.
    verifyOnWholeCluster(secondResumeAfterToken,
                         {$match: {ns: {db: dbName, coll: "coll1"}}},
                         {coll1: {drop: ["coll1", "coll1"]}},
                         1 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(secondResumeAfterToken,
                         {$match: {"ns.coll": "coll1"}},
                         {coll1: {drop: ["coll1", "coll1"]}},
                         1 /* expectedOplogRetDocsForEachShard */);
    verifyOnWholeCluster(secondResumeAfterToken,
                         {$match: {"ns.coll": /^col.*1/}},
                         {coll1: {drop: ["coll1", "coll1"]}},
                         1 /* expectedOplogRetDocsForEachShard */);

    verifyOnWholeCluster(
        secondResumeAfterToken,
        {$match: {ns: {db: dbName}}},
        {change_stream_match_pushdown_and_rewrite_and_rewrite: {dropDatabase: [dbName, dbName]}},
        1 /* expectedOplogRetDocsForEachShard */);
})();

st.stop();
})();
