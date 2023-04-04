// Test that a pipeline of the form [{$changeStream: {}}, {$match: <predicate>}] with a predicate
// involving the 'documentKey' field can push down the $match and rewrite the $match and make it
// part of the oplog cursor's filter in order to filter out results as early as possible.
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
load("jstests/libs/fixture_helpers.js");             // For FixtureHelpers.

const dbName = "change_stream_match_pushdown_documentKey_rewrite";
const collName = "change_stream_match_pushdown_documentKey_rewrite";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

// Returns a newly created sharded collection, where shard key is 'shard'.
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
        changeStreamSpec: {resumeAfter: resumeAfterToken},
        userMatchExpr: userMatchExpr,
        expectedResult: expectedResult,
        expectedOplogNReturnedPerShard: expectedOplogRetDocsForEachShard,
        expectedChangeStreamDocsReturnedPerShard: expectedChangeStreamDocsForEachShard
    });
}

// These operations will create oplog events. The change stream will apply several filters on these
// series of events and ensure that the '$match' expressions are rewritten correctly.
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
assert(coll.drop());

// Enable a failpoint that will prevent $expr match expressions from generating $_internalExprEq
// or similar expressions. This ensures that the following test-cases only exercise the $expr
// rewrites.
assert.commandWorked(
    db.adminCommand({configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));
FixtureHelpers.runCommandOnEachPrimary({
    db: db.getSiblingDB("admin"),
    cmdObj: {configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}
});

// Ensure that the '$match' on the 'insert', 'update', 'replace', and 'delete' operation types with
// various predicates are rewritten correctly.
for (const op of ["insert", "update", "replace", "delete"]) {
    // Test out a predicate on the full 'documentKey' field.
    verifyOnWholeCluster({$match: {operationType: op, documentKey: {shard: 0, _id: 2}}},
                         {[collName]: {[op]: [2]}},
                         [1, 0] /* expectedOplogRetDocsForEachShard */,
                         [1, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out a predicate on 'documentKey._id'.
    verifyOnWholeCluster({$match: {operationType: op, "documentKey._id": 2}},
                         {[collName]: {[op]: [2, 2]}},
                         [1, 1] /* expectedOplogRetDocsForEachShard */,
                         [1, 1] /* expectedChangeStreamDocsForEachShard */);

    // Test out a predicate on 'documentKey.shard'.
    verifyOnWholeCluster({$match: {operationType: op, "documentKey.shard": 1}},
                         {[collName]: {[op]: [2, 3]}},
                         [0, 2] /* expectedOplogRetDocsForEachShard */,
                         [0, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated predicate on the full 'documentKey' field.
    verifyOnWholeCluster(
        {$match: {operationType: op, documentKey: {$not: {$eq: {shard: 0, _id: 2}}}}},
        {[collName]: {[op]: [3, 2, 3]}},
        [1, 2] /* expectedOplogRetDocsForEachShard */,
        [1, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated predicate on 'documentKey._id'.
    verifyOnWholeCluster({$match: {operationType: op, "documentKey._id": {$not: {$eq: 2}}}},
                         {[collName]: {[op]: [3, 3]}},
                         [1, 1] /* expectedOplogRetDocsForEachShard */,
                         [1, 1] /* expectedChangeStreamDocsForEachShard */);

    // Test out an {$eq: null} predicate on 'documentKey._id'.
    verifyOnWholeCluster({$match: {operationType: op, "documentKey._id": {$eq: null}}},
                         {},
                         [0, 0] /* expectedOplogRetDocsForEachShard */,
                         [0, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated predicate on 'documentKey.shard'.
    verifyOnWholeCluster({$match: {operationType: op, "documentKey.shard": {$not: {$eq: 1}}}},
                         {[collName]: {[op]: [2, 3]}},
                         [2, 0] /* expectedOplogRetDocsForEachShard */,
                         [2, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out the '{$exists: false}' predicate on a field that doesn't exist in 'documentKey' but
    // that does exist in some of the underlying documents.
    verifyOnWholeCluster({$match: {operationType: op, "documentKey.z": {$exists: false}}},
                         {[collName]: {[op]: [2, 3, 2, 3]}},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [2, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out the '{$eq: null}' predicate on a field that doesn't exist in 'documentKey' but that
    // does exist in some of the underlying documents.
    verifyOnWholeCluster({$match: {operationType: op, "documentKey.z": {$eq: null}}},
                         {[collName]: {[op]: [2, 3, 2, 3]}},
                         [2, 2] /* expectedOplogRetDocsForEachShard */,
                         [2, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out an $expr predicate on the full 'documentKey' field.
    verifyOnWholeCluster({
        $match: {$and: [{operationType: op}, {$expr: {$eq: ["$documentKey", {shard: 0, _id: 2}]}}]}
    },
                         {[collName]: {[op]: [2]}},
                         [1, 0] /* expectedOplogRetDocsForEachShard */,
                         [1, 0] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated predicate on the full 'documentKey' field.
    verifyOnWholeCluster({
        $match: {
            $and:
                [{operationType: op}, {$expr: {$not: {$eq: ["$documentKey", {shard: 0, _id: 2}]}}}]
        }
    },
                         {[collName]: {[op]: [3, 2, 3]}},
                         [1, 2] /* expectedOplogRetDocsForEachShard */,
                         [1, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out an $expr predicate on 'documentKey._id'.
    verifyOnWholeCluster(
        {$match: {$and: [{operationType: op}, {$expr: {$eq: ["$documentKey._id", 2]}}]}},
        {[collName]: {[op]: [2, 2]}},
        [1, 1] /* expectedOplogRetDocsForEachShard */,
        [1, 1] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated $expr predicate on 'documentKey._id'.
    verifyOnWholeCluster(
        {$match: {$and: [{operationType: op}, {$expr: {$not: {$eq: ["$documentKey._id", 2]}}}]}},
        {[collName]: {[op]: [3, 3]}},
        [1, 1] /* expectedOplogRetDocsForEachShard */,
        [1, 1] /* expectedChangeStreamDocsForEachShard */);

    // Test out an $expr predicate on 'documentKey.shard'.
    verifyOnWholeCluster(
        {$match: {$and: [{operationType: op}, {$expr: {$eq: ["$documentKey.shard", 1]}}]}},
        {[collName]: {[op]: [2, 3]}},
        [0, 2] /* expectedOplogRetDocsForEachShard */,
        [0, 2] /* expectedChangeStreamDocsForEachShard */);

    // Test out a negated $expr predicate on 'documentKey.shard'.
    verifyOnWholeCluster(
        {$match: {$and: [{operationType: op}, {$expr: {$not: {$eq: ["$documentKey.shard", 1]}}}]}},
        {[collName]: {[op]: [2, 3]}},
        [2, 0] /* expectedOplogRetDocsForEachShard */,
        [2, 0] /* expectedChangeStreamDocsForEachShard */);
}

verifyOnWholeCluster({$match: {operationType: "drop"}},
                     {[collName]: {drop: [collName]}},
                     [1, 0] /* expectedOplogRetDocsForEachShard */,
                     [1, 0] /* expectedChangeStreamDocsForEachShard */);

st.stop();
})();
