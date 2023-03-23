// Test that a pipeline of the form [{$changeStream: {}}, {$match: <predicate>}] with a predicate
// involving the 'fullDocument' field can push down the $match and rewrite the $match and make it
// part of the oplog cursor's filter in order to filter out results as early as possible.
// @tags: [
//   featureFlagChangeStreamsRewrite,
//   requires_fcv_63,
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

const dbName = "change_stream_match_pushdown_fullDocument_rewrite";
const collName = "change_stream_match_pushdown_fullDocument_rewrite";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

// Create a sharded collection where shard key is 'shard'.
const coll = createShardedCollection(st, "shard" /* shardKey */, dbName, collName, 1 /* splitAt */);

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken = coll.watch([]).getResumeToken();

// A helper that opens a change stream on the whole cluster with the user supplied match expression
// 'userMatchExpr' and validates that:
// 1. for each shard, the events are seen in that order as specified in 'expectedResult'
// 2. the filtering is been done at oplog level
// 3. the number of docs returned by the oplog cursor on each shard matches what we expect
//    as specified in 'expectedOplogRetDocsForEachShard'.
// 4. the number of docs returned by each shard matches what we expect as specified by
//     'expectedChangeStreamDocsForEachShard'; and
function verifyOnWholeCluster(userMatchExpr,
                              expectedResult,
                              expectedOplogRetDocsForEachShard,
                              expectedChangeStreamDocsForEachShard) {
    verifyChangeStreamOnWholeCluster({
        st: st,
        changeStreamSpec: {resumeAfter: resumeAfterToken, fullDocument: "updateLookup"},
        userMatchExpr: userMatchExpr,
        expectedResult: expectedResult,
        expectedOplogNReturnedPerShard: expectedOplogRetDocsForEachShard,
        expectedChangeStreamDocsReturnedPerShard: expectedChangeStreamDocsForEachShard
    });
}

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
        // The 'delete' event never has a 'fullDocument' field, so we expect the same results
        // whether we are filtering on the field itself or a subfield.
        for (let fullDocumentPath of ["fullDocument", "fullDocument._id", "fullDocument.shard"]) {
            jsTestLog("Testing path '" + fullDocumentPath + "' for 'delete' events");

            // Test out the '{$exists: true}' predicate on the 'fullDocument' field.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$exists: true}}},
                                 {},
                                 [0, 0] /* expectedOplogRetDocsForEachShard */,
                                 [0, 0] /* expectedChangeStreamDocsForEachShard */);

            // Test out the '{$exists: false}' predicate on the 'fullDocument' field.
            verifyOnWholeCluster(
                {$match: {operationType: op, [fullDocumentPath]: {$exists: false}}},
                {[collName]: {[op]: [2, 3, 2, 3]}},
                [2, 2] /* expectedOplogRetDocsForEachShard */,
                [2, 2] /* expectedChangeStreamDocsForEachShard */);

            // Test out the '{$eq: null}' predicate on the 'fullDocument' field.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$eq: null}}},
                                 {[collName]: {[op]: [2, 3, 2, 3]}},
                                 [2, 2] /* expectedOplogRetDocsForEachShard */,
                                 [2, 2] /* expectedChangeStreamDocsForEachShard */);

            // Test out the '{$ne: null}' predicate on the 'fullDocument' field. We cannot perform
            // an exact rewrite of this negated predicate on 'fullDocument', so the oplog scan
            // returns all 'delete' events and we subsequently filter them out in the pipeline.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$ne: null}}},
                                 {},
                                 [2, 2] /* expectedOplogRetDocsForEachShard */,
                                 [0, 0] /* expectedChangeStreamDocsForEachShard */);

            // Test out an inequality on null for the 'fullDocument' field.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$gt: null}}},
                                 {},
                                 [0, 0] /* expectedOplogRetDocsForEachShard */,
                                 [0, 0] /* expectedChangeStreamDocsForEachShard */);

            // Test out a negated inequality on null for the 'fullDocument' field.
            verifyOnWholeCluster(
                {$match: {operationType: op, [fullDocumentPath]: {$not: {$gt: null}}}},
                {[collName]: {[op]: [2, 3, 2, 3]}},
                [2, 2] /* expectedOplogRetDocsForEachShard */,
                [2, 2] /* expectedChangeStreamDocsForEachShard */);

            // We expect the same results for $lte as we got for {$not: {$gt}}, although we can
            // rewrite this predicate into the oplog.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$lte: null}}},
                                 {[collName]: {[op]: [2, 3, 2, 3]}},
                                 [2, 2] /* expectedOplogRetDocsForEachShard */,
                                 [2, 2] /* expectedChangeStreamDocsForEachShard */);

            // Test that {$type: 'null'} on the 'fullDocument' field does not match.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$type: "null"}}},
                                 {},
                                 [0, 0] /* expectedOplogRetDocsForEachShard */,
                                 [0, 0] /* expectedChangeStreamDocsForEachShard */);

            // Test that negated {$type: 'null'} on the 'fullDocument' field matches.
            verifyOnWholeCluster(
                {$match: {operationType: op, [fullDocumentPath]: {$not: {$type: "null"}}}},
                {[collName]: {[op]: [2, 3, 2, 3]}},
                [2, 2] /* expectedOplogRetDocsForEachShard */,
                [2, 2] /* expectedChangeStreamDocsForEachShard */);

            // Test out a non-null non-$exists predicate on the 'fullDocument' field.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$eq: 5}}},
                                 {},
                                 [0, 0] /* expectedOplogRetDocsForEachShard */,
                                 [0, 0] /* expectedChangeStreamDocsForEachShard */);

            // Test out a negated non-null non-$exists predicate on the 'fullDocument' field.
            verifyOnWholeCluster({$match: {operationType: op, [fullDocumentPath]: {$ne: 5}}},
                                 {[collName]: {[op]: [2, 3, 2, 3]}},
                                 [2, 2] /* expectedOplogRetDocsForEachShard */,
                                 [2, 2] /* expectedChangeStreamDocsForEachShard */);
        }
        return;
    }

    // Non-CRUD events don't have a 'fullDocument' field, so test 'drop' separately. We run these
    // tests on the whole DB because otherwise the stream will be invalidated, and it's impossible
    // to tell whether we will see one drop or two, or from which shard.
    if (op == "drop") {
        // Test that {$eq: null} on the 'fullDocument' field matches the 'drop' event.
        verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$eq: null}}},
                             {[collName]: {[op]: [collName]}},
                             [1, 0] /* expectedOplogRetDocsForEachShard */,
                             [1, 0] /* expectedChangeStreamDocsForEachShard */);

        // Test that {$exists: false} on the 'fullDocument' field matches the 'drop' event.
        verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$exists: false}}},
                             {[collName]: {[op]: [collName]}},
                             [1, 0] /* expectedOplogRetDocsForEachShard */,
                             [1, 0] /* expectedChangeStreamDocsForEachShard */);

        // Test that {$exists: true} on the 'fullDocument' field does not match the 'drop' event.
        verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$exists: true}}},
                             {},
                             [0, 0] /* expectedOplogRetDocsForEachShard */,
                             [0, 0] /* expectedChangeStreamDocsForEachShard */);
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
    verifyOnWholeCluster({$match: {operationType: op, fullDocument: doc}},
                         {[collName]: {[op]: [2]}},
                         op != "update" ? [1, 0] : [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [1, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out a predicate on 'fullDocument._id'.
    verifyOnWholeCluster({$match: {operationType: op, "fullDocument._id": {$lt: 3}}},
                         {[collName]: {[op]: [2, 2]}},
                         op != "update" ? [1, 1] : [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [1, 1] /* expectedChangeStreamDocsForEachShard */);

    // Test out a predicate on 'fullDocument.shard'.
    verifyOnWholeCluster({$match: {operationType: op, "fullDocument.shard": {$gt: 0}}},
                         {[collName]: {[op]: [2, 3]}},
                         op != "update" ? [0, 2] : [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [0, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated predicate on the full 'fullDocument' field.
    verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$not: {$eq: doc}}}},
                         {[collName]: {[op]: [3, 2, 3]}},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [1, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated predicate on 'fullDocument._id'.
    verifyOnWholeCluster({$match: {operationType: op, "fullDocument._id": {$not: {$lt: 3}}}},
                         {[collName]: {[op]: [3, 3]}},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [1, 1] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated predicate on 'fullDocument.shard'.
    verifyOnWholeCluster({$match: {operationType: op, "fullDocument.shard": {$not: {$gt: 0}}}},
                         {[collName]: {[op]: [2, 3]}},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [2, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out the '{$exists: true}' predicate on the full 'fullDocument' field.
    verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$exists: true}}},
                         {[collName]: {[op]: [2, 3, 2, 3]}},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [2, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out the '{$exists: false}' predicate on the full 'fullDocument' field.
    verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$exists: false}}},
                         {},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [0, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out the '{$eq: null}' predicate on the 'fullDocument' field.
    verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$eq: null}}},
                         {},
                         op != "update" ? [0, 0] : [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [0, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out the '{$ne: null}' predicate on the 'fullDocument' field.
    verifyOnWholeCluster({$match: {operationType: op, fullDocument: {$ne: null}}},
                         {[collName]: {[op]: [2, 3, 2, 3]}},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [2, 2] /* expectedChangeStreamDocsForEachShard */);
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

// Now drop the collection and verify that we see the 'drop' event with no 'fullDocument'.
assert(coll.drop());
runVerifyOpsTestcases("drop");

st.stop();
})();
