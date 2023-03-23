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

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken = coll.watch([]).getResumeToken();

// A helper that opens a change stream on the whole cluster with the user supplied match expression
// 'userMatchExpr' and validates that:
// 1. for each shard, the events are seen in that order as specified in 'expectedResult'
// 2. the filtering is been done at oplog level
// 3. the number of docs returned by the oplog cursor on each shard matches what we expect
//    as specified in 'expectedOplogRetDocsForEachShard'.
function verifyOnWholeCluster(userMatchExpr, expectedResult, expectedOplogRetDocsForEachShard) {
    verifyChangeStreamOnWholeCluster({
        st: st,
        changeStreamSpec: {resumeAfter: resumeAfterToken, showExpandedEvents: true},
        userMatchExpr: userMatchExpr,
        expectedResult: expectedResult,
        expectedOplogNReturnedPerShard: Array.isArray(expectedOplogRetDocsForEachShard)
            ? expectedOplogRetDocsForEachShard
            : [expectedOplogRetDocsForEachShard, expectedOplogRetDocsForEachShard],
    });
}

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

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(
    coll.runCommand({collMod: coll.getName(), index: {keyPattern: {a: 1}, hidden: true}}));
assert.commandWorked(coll.dropIndex({a: 1}));
assert(coll.drop());

// Ensure that the '$match' on the 'insert' operation type is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: "insert"}},
                     {[collName]: {"insert": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the 'update' operation type is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: "update"}},
                     {[collName]: {"update": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the 'replace' operation type is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: "replace"}},
                     {[collName]: {"replace": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the 'delete' operation type is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: "delete"}},
                     {[collName]: {"delete": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the operation type as number is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: 1}}, {}, 0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the operation type unknown is rewritten correctly.
verifyOnWholeCluster(
    {$match: {operationType: "unknown"}}, {}, 0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on an empty string operation type is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: ""}}, {}, 0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on operation type with inequality operator cannot be rewritten to the
// oplog format. The oplog cursor should return all documents for each shard.
verifyOnWholeCluster(
    {$match: {operationType: {$gt: "insert"}}},
    {[collName]: {"update": [1, 2], "replace": [1, 2], "modify": [[collName], [collName]]}},
    [8, 7] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on operation type sub-field can be rewritten to the oplog format. The
// oplog cursor should return '0' documents for each shard.
verifyOnWholeCluster({$match: {"operationType.subField": "subOperation"}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$eq: null' on operation type sub-field can be rewritten to the oplog format. The
// oplog cursor should return all documents for each shard.
verifyOnWholeCluster({$match: {"operationType.subField": {$eq: null}}},
                     {
                         [collName]: {
                             "insert": [1, 2],
                             "update": [1, 2],
                             "replace": [1, 2],
                             "delete": [1, 2],
                             "createIndexes": [[collName], [collName]],
                             "modify": [[collName], [collName]],
                             "dropIndexes": [[collName], [collName]],
                             "drop": [[collName]]
                         }
                     },
                     [8, 7] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the operation type with '$in' is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: {$in: ["insert", "update"]}}},
                     {
                         [collName]: {
                             "insert": [1, 2],
                             "update": [1, 2],
                         }
                     },
                     2 /* expectedOplogRetDocsForEachShard */);

// Ensure that for the '$in' with one valid and one invalid operation type is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: {$in: ["insert", "unknown"]}}},
                     {[collName]: {"insert": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$in' on an unknown operation type is rewritten correctly.
verifyOnWholeCluster(
    {$match: {operationType: {$in: ["unknown"]}}}, {}, 0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$in' with operation type as number is rewritten correctly.
verifyOnWholeCluster(
    {$match: {operationType: {$in: [1]}}}, {}, 0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$in' with operation type as a string and a regex cannot be
// rewritten. The oplog cursor should return '4' documents for each shard.
verifyOnWholeCluster({$match: {operationType: {$in: [/^insert$/, "update"]}}},
                     {[collName]: {"insert": [1, 2], "update": [1, 2]}},
                     [8, 7] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the operation type with '$nin' is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: {$nin: ["insert"]}}},
                     {
                         [collName]: {
                             "update": [1, 2],
                             "replace": [1, 2],
                             "delete": [1, 2],
                             "createIndexes": [[collName], [collName]],
                             "modify": [[collName], [collName]],
                             "dropIndexes": [[collName], [collName]],
                             "drop": [[collName]]
                         }
                     },
                     [7, 6] /* expectedOplogRetDocsForEachShard */);

// Ensure that for the '$nin' with one valid and one invalid operation type is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: {$nin: ["insert", "unknown"]}}},
                     {
                         [collName]: {
                             "update": [1, 2],
                             "replace": [1, 2],
                             "delete": [1, 2],
                             "createIndexes": [[collName], [collName]],
                             "modify": [[collName], [collName]],
                             "dropIndexes": [[collName], [collName]],
                             "drop": [[collName]]
                         }
                     },
                     [7, 6] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' with '$nin' with operation type as number is rewritten correctly.
verifyOnWholeCluster({$match: {operationType: {$nin: [1]}}},
                     {
                         [collName]: {
                             "insert": [1, 2],
                             "update": [1, 2],
                             "replace": [1, 2],
                             "delete": [1, 2],
                             "createIndexes": [[collName], [collName]],
                             "modify": [[collName], [collName]],
                             "dropIndexes": [[collName], [collName]],
                             "drop": [[collName]]
                         }
                     },
                     [8, 7] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'insert' operations is rewritten correctly.
verifyOnWholeCluster({$match: {$expr: {$eq: ["$operationType", "insert"]}}},
                     {[collName]: {"insert": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'update' operations is rewritten correctly.
verifyOnWholeCluster({$match: {$expr: {$eq: ["$operationType", "update"]}}},
                     {[collName]: {"update": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'replace' operations is rewritten correctly.
verifyOnWholeCluster({$match: {$expr: {$eq: ["$operationType", "replace"]}}},
                     {[collName]: {"replace": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' to match only 'delete' operations is rewritten correctly.
verifyOnWholeCluster({$match: {$expr: {$eq: ["$operationType", "delete"]}}},
                     {[collName]: {"delete": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when comparing with 'unknown'
// operation type.
verifyOnWholeCluster({$match: {$expr: {$eq: ["$operationType", "unknown"]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when '$and' is in the expression.
verifyOnWholeCluster({
    $match: {
        $expr: {
            $and: [
                {$gte: [{$indexOfCP: ["$operationType", "l"]}, 0]},
                {$gte: [{$indexOfCP: ["$operationType", "te"]}, 0]}
            ]
        }
    }
},
                     {[collName]: {"delete": [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when '$or' is in the expression.
verifyOnWholeCluster({
    $match: {
        $expr: {
            $or: [
                {$gte: [{$indexOfCP: ["$operationType", "l"]}, 0]},
                {$gte: [{$indexOfCP: ["$operationType", "te"]}, 0]}
            ]
        }
    }
},
                     {
                         [collName]: {
                             "update": [1, 2],
                             "replace": [1, 2],
                             "delete": [1, 2],
                             "createIndexes": [[collName], [collName]]
                         }
                     },
                     4 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when '$not' is in the expression.
verifyOnWholeCluster(
    {$match: {$expr: {$not: {$regexMatch: {input: "$operationType", regex: /e$/}}}}},
    {
        [collName]: {
            "insert": [1, 2],
            "createIndexes": [[collName], [collName]],
            "modify": [[collName], [collName]],
            "dropIndexes": [[collName], [collName]],
            "drop": [[collName]]
        }
    },
    [5, 4] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' using '$expr' is rewritten correctly when nor ({$not: {$or: [...]}}) is
// in the expression.
verifyOnWholeCluster({
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
                     {
                         [collName]: {
                             "update": [1, 2],
                             "replace": [1, 2],
                             "createIndexes": [[collName], [collName]],
                             "modify": [[collName], [collName]],
                             "dropIndexes": [[collName], [collName]],
                             "drop": [[collName]]
                         }
                     },
                     [6, 5] /* expectedOplogRetDocsForEachShard */);

st.stop();
})();
