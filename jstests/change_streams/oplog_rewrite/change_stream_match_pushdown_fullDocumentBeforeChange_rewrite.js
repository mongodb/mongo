// Test that a pipeline of the form [{$changeStream: {}}, {$match: <predicate>}] with a predicate
// involving the 'fullDocumentBeforeChange' field can push down the $match and rewrite the $match
// and make it part of the oplog cursor's filter in order to filter out results as early as
// possible.
// @tags: [
//   featureFlagChangeStreamsRewrite,
//   requires_fcv_61,
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

const dbName = "change_stream_match_pushdown_fullDocumentBeforeChange_rewrite";
const collName = "change_stream_match_pushdown_fullDocumentBeforeChange_rewrite";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

// Create a sharded collection where shard key is 'shard'.
const coll = createShardedCollection(st, "shard" /* shardKey */, dbName, collName, 1 /* splitAt */);
const testDB = st.s.getDB(dbName);

// Enable change stream pre-images on the test collection.
assert.commandWorked(
    testDB.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}));

// A helper that opens a change stream with the user supplied match expression 'userMatchExpr' and
// validates that:
// (1) for each shard, the events are seen in that order as specified in 'expectedOps'; and
// (2) the number of docs returned by each shard matches what we expect as specified by
//     'expectedChangeStreamDocsReturned'; and
// (3) the number of docs returned by the oplog cursor on each shard matches what we expect as
//     specified in 'expectedOplogCursorReturnedDocs'.
function verifyOps(resumeAfterToken,
                   userMatchExpr,
                   expectedOps,
                   expectedChangeStreamDocsReturned,
                   expectedOplogCursorReturnedDocs,
                   runOnWholeDB) {
    const cursor = (runOnWholeDB ? testDB : coll).aggregate([
        {$changeStream: {resumeAfter: resumeAfterToken, fullDocumentBeforeChange: "whenAvailable"}},
        userMatchExpr
    ]);

    for (const [op, id, shardId] of expectedOps) {
        assert.soon(() => cursor.hasNext());
        const event = cursor.next();
        assert.eq(event.operationType, op, event);
        if (id !== undefined) {
            assert.eq(event.fullDocumentBeforeChange._id, id, event);
        }
        if (shardId !== undefined) {
            assert.eq(event.fullDocumentBeforeChange.shard, shardId, event);
        }
    }

    assert(!cursor.hasNext());

    // An 'executionStats' could only be captured for a non-invalidating stream.
    const stats = assert.commandWorked(testDB.runCommand({
        explain: {
            aggregate: (runOnWholeDB ? 1 : coll.getName()),
            pipeline: [
                {
                    $changeStream:
                        {resumeAfter: resumeAfterToken, fullDocumentBeforeChange: "whenAvailable"}
                },
                userMatchExpr
            ],
            cursor: {}
        },
        verbosity: "executionStats"
    }));

    assertNumChangeStreamDocsReturnedFromShard(
        stats, st.rs0.name, expectedChangeStreamDocsReturned[0]);
    assertNumChangeStreamDocsReturnedFromShard(
        stats, st.rs1.name, expectedChangeStreamDocsReturned[1]);
    assertNumMatchingOplogEventsForShard(stats, st.rs0.name, expectedOplogCursorReturnedDocs[0]);
    assertNumMatchingOplogEventsForShard(stats, st.rs1.name, expectedOplogCursorReturnedDocs[1]);
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
assert.commandWorked(coll.deleteOne({_id: 2, shard: 0}));
assert.commandWorked(coll.deleteOne({_id: 3, shard: 0}));
assert.commandWorked(coll.deleteOne({_id: 2, shard: 1}));
assert.commandWorked(coll.deleteOne({_id: 3, shard: 1}));

// This helper takes an operation 'op' and calls verifyOps() multiple times with 'op' to exercise
// several different testcases.
const runVerifyOpsTestcases = (op) => {
    // 'insert' operations don't have a 'fullDocumentBeforeChange' field, so we handle them as a
    // special case.
    if (op == "insert") {
        // The 'insert' event never has a 'fullDocumentBeforeChange' field, so we expect the same
        // results whether we are filtering on the field itself or a subfield.
        for (let fullDocumentBeforeChangePath of ["fullDocumentBeforeChange",
                                                  "fullDocumentBeforeChange._id",
                                                  "fullDocumentBeforeChange.shard"]) {
            // Test out the '{$exists: true}' predicate on the 'fullDocumentBeforeChange' field.
            verifyOps(
                resumeAfterToken,
                {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$exists: true}}},
                [],
                [0, 0] /* expectedChangeStreamDocsReturned */,
                [0, 0] /* expectedOplogCursorReturnedDocs */);

            // Test out the '{$exists: false}' predicate on the 'fullDocumentBeforeChange' field.
            verifyOps(
                resumeAfterToken,
                {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$exists: false}}},
                [[op], [op], [op], [op]],
                [2, 2] /* expectedChangeStreamDocsReturned */,
                [2, 2] /* expectedOplogCursorReturnedDocs */);

            // Test out the '{$eq: null}' predicate on the 'fullDocumentBeforeChange' field.
            verifyOps(resumeAfterToken,
                      {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$eq: null}}},
                      [[op], [op], [op], [op]],
                      [2, 2] /* expectedChangeStreamDocsReturned */,
                      [2, 2] /* expectedOplogCursorReturnedDocs */);

            // Test out the '{$ne: null}' predicate on the 'fullDocumentBeforeChange' field. We
            // cannot perform an exact rewrite of this negated predicate on
            // 'fullDocumentBeforeChange', so the oplog scan returns all 'insert' events and we
            // subsequently filter them out in the pipeline.
            verifyOps(resumeAfterToken,
                      {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$ne: null}}},
                      [],
                      [0, 0] /* expectedChangeStreamDocsReturned */,
                      [2, 2] /* expectedOplogCursorReturnedDocs */);

            // Test out an inequality on null for the 'fullDocumentBeforeChange' field.
            verifyOps(resumeAfterToken,
                      {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$gt: null}}},
                      [],
                      [0, 0] /* expectedChangeStreamDocsReturned */,
                      [0, 0] /* expectedOplogCursorReturnedDocs */);

            // Test out a negated inequality on null for the 'fullDocumentBeforeChange' field.
            verifyOps(
                resumeAfterToken,
                {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$not: {$gt: null}}}},
                [[op], [op], [op], [op]],
                [2, 2] /* expectedChangeStreamDocsReturned */,
                [2, 2] /* expectedOplogCursorReturnedDocs */);

            // We expect the same results for $lte as we got for {$not: {$gt}}, although we can
            // rewrite this predicate into the oplog.
            verifyOps(resumeAfterToken,
                      {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$lte: null}}},
                      [[op], [op], [op], [op]],
                      [2, 2] /* expectedChangeStreamDocsReturned */,
                      [2, 2] /* expectedOplogCursorReturnedDocs */);

            // Test that {$type: 'null'} on the 'fullDocumentBeforeChange' field does not match.
            verifyOps(
                resumeAfterToken,
                {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$type: "null"}}},
                [],
                [0, 0] /* expectedChangeStreamDocsReturned */,
                [0, 0] /* expectedOplogCursorReturnedDocs */);

            // Test that negated {$type: 'null'} on the 'fullDocumentBeforeChange' field matches.
            verifyOps(
                resumeAfterToken,
                {
                    $match:
                        {operationType: op, [fullDocumentBeforeChangePath]: {$not: {$type: "null"}}}
                },
                [[op], [op], [op], [op]],
                [2, 2] /* expectedChangeStreamDocsReturned */,
                [2, 2] /* expectedOplogCursorReturnedDocs */);

            // Test out a non-null non-$exists predicate on the 'fullDocumentBeforeChange' field.
            verifyOps(resumeAfterToken,
                      {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$eq: 5}}},
                      [],
                      [0, 0] /* expectedChangeStreamDocsReturned */,
                      [0, 0] /* expectedOplogCursorReturnedDocs */);

            // Test out a negated non-null non-$exists predicate on the 'fullDocumentBeforeChange'
            // field.
            verifyOps(resumeAfterToken,
                      {$match: {operationType: op, [fullDocumentBeforeChangePath]: {$ne: 5}}},
                      [[op], [op], [op], [op]],
                      [2, 2] /* expectedChangeStreamDocsReturned */,
                      [2, 2] /* expectedOplogCursorReturnedDocs */);
        }
        return;
    }

    // Initialize 'doc' so that it matches the 'fullDocumentBeforeChange' field of one of the events
    // where operationType == 'op'. For all 'replace' events, 'fullDocumentBeforeChange' only has
    // the '_id' field and the 'shard' field. For 'delete' and 'update' events,
    // 'fullDocumentBeforeChange' also has a 'foo' field.
    const doc = {_id: 2, shard: 0};
    if (op == "update") {
        doc.foo = "a";
    }
    if (op == "delete") {
        doc.foo = "b";
    }

    // Test out a predicate on the full 'fullDocumentBeforeChange' field.
    jsTestLog("Testing 'fullDocumentBeforeChange' for op: " + op +
              ", expected pre-image: " + tojsononeline(doc));
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocumentBeforeChange: doc}},
              [[op, 2, 0]],
              [1, 0] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a predicate on 'fullDocumentBeforeChange._id'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocumentBeforeChange._id": {$lt: 3}}},
              [[op, 2, 0], [op, 2, 1]],
              [1, 1] /* expectedChangeStreamDocsReturned */,
              [1, 1] /* expectedOplogCursorReturnedDocs */);

    // Test out a predicate on 'fullDocumentBeforeChange.shard'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocumentBeforeChange.shard": {$gt: 0}}},
              [[op, 2, 1], [op, 3, 1]],
              [0, 2] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a negated predicate on the full 'fullDocumentBeforeChange' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocumentBeforeChange: {$not: {$eq: doc}}}},
              [[op, 3, 0], [op, 2, 1], [op, 3, 1]],
              [1, 2] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a negated predicate on 'fullDocumentBeforeChange._id'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocumentBeforeChange._id": {$not: {$lt: 3}}}},
              [[op, 3, 0], [op, 3, 1]],
              [1, 1] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out a negated predicate on 'fullDocumentBeforeChange.shard'.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, "fullDocumentBeforeChange.shard": {$not: {$gt: 0}}}},
              [[op, 2, 0], [op, 3, 0]],
              [2, 0] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out the '{$exists: true}' predicate on the full 'fullDocumentBeforeChange' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocumentBeforeChange: {$exists: true}}},
              [[op, 2, 0], [op, 3, 0], [op, 2, 1], [op, 3, 1]],
              [2, 2] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out the '{$exists: false}' predicate on the full 'fullDocumentBeforeChange' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocumentBeforeChange: {$exists: false}}},
              [],
              [0, 0] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out the '{$eq: null}' predicate on the 'fullDocumentBeforeChange' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocumentBeforeChange: {$eq: null}}},
              [],
              [0, 0] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);

    // Test out the '{$ne: null}' predicate on the 'fullDocumentBeforeChange' field.
    verifyOps(resumeAfterToken,
              {$match: {operationType: op, fullDocumentBeforeChange: {$ne: null}}},
              [[op, 2, 0], [op, 3, 0], [op, 2, 1], [op, 3, 1]],
              [2, 2] /* expectedChangeStreamDocsReturned */,
              [2, 2] /* expectedOplogCursorReturnedDocs */);
};

// Ensure that '$match' on 'insert', 'replace', and 'delete' operation types with various predicates
// are rewritten correctly.
runVerifyOpsTestcases("insert");
runVerifyOpsTestcases("replace");
runVerifyOpsTestcases("update");
runVerifyOpsTestcases("delete");

// Now drop the collection.
assert(coll.drop());

st.stop();
})();
