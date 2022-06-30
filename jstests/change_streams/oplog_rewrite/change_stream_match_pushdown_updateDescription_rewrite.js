// Test that a pipeline of the form [{$changeStream: {}}, {$match: <predicate>}] with a predicate
// involving the 'updateDescription' field can push down the $match and rewrite the $match and make
// it part of the oplog cursor's filter in order to filter out results as early as possible.
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

const dbName = "change_stream_match_pushdown_updateDescription_rewrite";
const collName = "change_stream_match_pushdown_updateDescription_rewrite";

// Start a new 2-shard cluster.
const st = new ShardingTest({
    shards: [
        {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        {
            nodes: 1,
            setParameter: {
                writePeriodicNoops: true,
                periodicNoopIntervalSecs: 1,
            }
        }
    ]
});

const s0 = 0;
const s1 = 1;

// Returns a newly created sharded collection, where shard key is 'shard'.
const coll = createShardedCollection(st, "shard" /* shardKey */, dbName, collName, 1 /* splitAt */);

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken = coll.watch([]).getResumeToken();

// A helper that opens a change stream with the user supplied match expression 'userMatchExpr'
// and validates that: (1) for each shard, the events are seen in that order as specified in
// 'expectedOps'; and (2) each shard returns the expected number of events; and (3) the number
// of docs returned by the oplog cursor on each shard matches what we expect
// as specified in 'expectedOplogCursorReturnedDocs'.
const verifyOps = function(userMatchExpr, expectedOps, expectedOplogCursorReturnedDocs) {
    const cursor =
        coll.aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr]);

    let expectedChangeStreamDocsReturned = [0, 0];
    for (const [op, id, s] of expectedOps) {
        const shardId = s == 0 ? s0 : s == 1 ? s1 : s;

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

    // An 'executionStats' could only be captured for a non-invalidating stream.
    const stats = coll.explain("executionStats")
                      .aggregate([{$changeStream: {resumeAfter: resumeAfterToken}}, userMatchExpr]);

    assertNumChangeStreamDocsReturnedFromShard(
        stats, st.rs0.name, expectedChangeStreamDocsReturned[0]);
    assertNumChangeStreamDocsReturnedFromShard(
        stats, st.rs1.name, expectedChangeStreamDocsReturned[1]);

    assertNumMatchingOplogEventsForShard(stats, st.rs0.name, expectedOplogCursorReturnedDocs[s0]);
    assertNumMatchingOplogEventsForShard(stats, st.rs1.name, expectedOplogCursorReturnedDocs[s1]);
};

// These operations will create oplog events. The change stream will apply several filters
// on these series of events and ensure that the '$match' expressions are rewritten
// correctly.
assert.commandWorked(coll.insert({_id: 2, shard: s0}));
assert.commandWorked(coll.insert({_id: 2, shard: s1}));
assert.commandWorked(coll.insert({_id: 3, shard: s0}));
assert.commandWorked(coll.insert({_id: 3, shard: s1}));

assert.commandWorked(
    coll.replaceOne({_id: 2, shard: s0}, {_id: 2, shard: s0, z: 4, f: "a", w: {h: 5, k: 5, l: 5}}));
assert.commandWorked(
    coll.replaceOne({_id: 2, shard: s1}, {_id: 2, shard: s1, z: 4, f: "a", w: {h: 5, k: 5, l: 5}}));
assert.commandWorked(
    coll.replaceOne({_id: 3, shard: s0}, {_id: 3, shard: s0, 1: 4, f: "a", w: {h: 5, k: 5, l: 5}}));
assert.commandWorked(
    coll.replaceOne({_id: 3, shard: s1}, {_id: 3, shard: s1, y: 4, f: "a", w: {h: 5, k: 5, l: 5}}));

assert.commandWorked(coll.update({_id: 2, shard: s0}, {$unset: {z: 0}}));
assert.commandWorked(coll.update({_id: 2, shard: s1}, [{$set: {g: "c"}}, {$unset: "z"}]));

assert.commandWorked(
    coll.update({_id: 3, shard: s0}, {$set: {f: "b", x: {j: 7}}, $unset: {"w.h": 0, 1: 0}}));
assert.commandWorked(
    coll.update({_id: 3, shard: s1}, {$set: {"0": "d", x: {j: 7}}, $unset: {y: 0, "w.h": 0}}));

assert.commandWorked(coll.deleteOne({_id: 2, shard: s0}));
assert.commandWorked(coll.deleteOne({_id: 2, shard: s1}));
assert.commandWorked(coll.deleteOne({_id: 3, shard: s0}));
assert.commandWorked(coll.deleteOne({_id: 3, shard: s1}));

// Ensure that the '$match' on the 'update' operation type with various predicates are rewritten
// correctly.
const op = "update";
const updateDesc = {
    updatedFields: {},
    removedFields: ["z"],
    truncatedArrays: []
};

// Test out a predicate on the full 'updateDescription' field.
verifyOps({$match: {operationType: op, updateDescription: updateDesc}},
          [[op, 2, 0]],
          [2, 2] /* expectedOplogCursorReturnedDocs */);

// Test out an $eq:null predicate on the full 'updateDescription' field.
verifyOps({$match: {operationType: op, updateDescription: {$eq: null}}},
          [],
          [0, 0] /* expectedOplogCursorReturnedDocs */);

// Test out a negated $exists predicate on the full 'updateDescription' field.
verifyOps({$match: {operationType: op, updateDescription: {$exists: false}}},
          [],
          [0, 0] /* expectedOplogCursorReturnedDocs */);

// Test out an $eq:null predicate on 'updateDescription.updatedFields'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields": {$eq: null}}},
          [],
          [0, 0] /* expectedOplogCursorReturnedDocs */);

// Test out a negated $exists predicate on 'updateDescription.updatedFields'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields": {$exists: false}}},
          [],
          [0, 0] /* expectedOplogCursorReturnedDocs */);

// Test out an $eq predicate on 'updateDescription.updatedFields.f'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.f": "b"}},
          [[op, 3, 0]],
          [1, 0] /* expectedOplogCursorReturnedDocs */);

// Test out an $lte predicate on 'updateDescription.updatedFields.f'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.f": {$lte: "b"}}},
          [[op, 3, 0]],
          [1, 0] /* expectedOplogCursorReturnedDocs */);

// Test out an $eq predicate on 'updateDescription.updatedFields.g'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.g": "c"}},
          [[op, 2, 1]],
          [0, 1] /* expectedOplogCursorReturnedDocs */);

// Test out an $exists predicate on 'updateDescription.updatedFields.g'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.g": {$exists: true}}},
          [[op, 2, 1]],
          [0, 1] /* expectedOplogCursorReturnedDocs */);

// Test out an $eq predicate on 'updateDescription.updatedFields.x.j'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.x.j": 7}},
          [[op, 3, 0], [op, 3, 1]],
          [2, 2] /* expectedOplogCursorReturnedDocs */);

// Test out an $eq predicate on 'updateDescription.updatedFields.0'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.0": "d"}},
          [[op, 3, 1]],
          [0, 1] /* expectedOplogCursorReturnedDocs */);

// Test out an $eq:null predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": {$eq: null}}},
          [],
          [0, 0] /* expectedOplogCursorReturnedDocs */);

// Test out a negated $exists predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": {$exists: false}}},
          [],
          [0, 0] /* expectedOplogCursorReturnedDocs */);

// Test out a non-dotted string $eq predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": "z"}},
          [[op, 2, 0], [op, 2, 1]],
          [1, 1] /* expectedOplogCursorReturnedDocs */);

// Test out an array $eq predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": ["z"]}},
          [[op, 2, 0], [op, 2, 1]],
          [2, 2] /* expectedOplogCursorReturnedDocs */);

// Test out a dotted string $eq predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": "w.h"}},
          [[op, 3, 0], [op, 3, 1]],
          [2, 2] /* expectedOplogCursorReturnedDocs */);

// Test out a number-like string $eq predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": "1"}},
          [[op, 3, 0]],
          [1, 0] /* expectedOplogCursorReturnedDocs */);

// Test out a non-dotted string $eq predicate on 'updateDescription.removedFields.0'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields.0": "z"}},
          [[op, 2, 0], [op, 2, 1]],
          [2, 2] /* expectedOplogCursorReturnedDocs */);

// Test out an $in predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": {$in: ["y", "z"]}}},
          [[op, 2, 0], [op, 2, 1], [op, 3, 1]],
          [1, 2] /* expectedOplogCursorReturnedDocs */);

// Test out a negated predicate on the full 'updateDescription' field.
verifyOps({$match: {operationType: op, updateDescription: {$not: {$eq: updateDesc}}}},
          [[op, 2, 1], [op, 3, 0], [op, 3, 1]],
          [2, 2] /* expectedOplogCursorReturnedDocs */);

// Test out a negated $eq predicate on 'updateDescription.updatedFields.f'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.f": {$not: {$eq: "b"}}}},
          [[op, 2, 0], [op, 2, 1], [op, 3, 1]],
          [1, 2] /* expectedOplogCursorReturnedDocs */);

// Test out a negated $exists predicate on 'updateDescription.updatedFields.g'.
verifyOps(
    {$match: {operationType: op, "updateDescription.updatedFields.g": {$not: {$exists: true}}}},
    [[op, 2, 0], [op, 3, 0], [op, 3, 1]],
    [2, 1] /* expectedOplogCursorReturnedDocs */);

// Test out an {$eq:null} predicate on 'updateDescription.updatedFields.g'.
verifyOps({$match: {operationType: op, "updateDescription.updatedFields.g": {$eq: null}}},
          [[op, 2, 0], [op, 3, 0], [op, 3, 1]],
          [2, 1] /* expectedOplogCursorReturnedDocs */);

// Test out a negated $eq predicate on 'updateDescription.removedFields'.
verifyOps({$match: {operationType: op, "updateDescription.removedFields": {$not: {$eq: "z"}}}},
          [[op, 3, 0], [op, 3, 1]],
          [1, 1] /* expectedOplogCursorReturnedDocs */);

// Test out a negated $in predicate on 'updateDescription.removedFields'.
verifyOps(
    {$match: {operationType: op, "updateDescription.removedFields": {$not: {$in: ["y", "z"]}}}},
    [[op, 3, 0]],
    [1, 0] /* expectedOplogCursorReturnedDocs */);

st.stop();
})();
