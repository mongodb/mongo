// Test that a pipeline of the form [{$changeStream: {}}, {$match: ...}] can rewrite the 'namespace'
// and apply it to oplog-format documents in order to filter out results as early as possible.
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

const dbName = "change_stream_match_pushdown_and_rewrite";
const otherDbName = "other_db";
const collName = "coll1";
const otherCollName = "other_coll";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

// Create a sharded collection in the main test database.
const coll = createShardedCollection(st, "_id" /* shardKey */, dbName, collName, 2 /* splitAt */);

// Create a sharded collection in the "other" database.
const otherColl =
    createShardedCollection(st, "_id" /* shardKey */, otherDbName, otherCollName, 2 /* splitAt */);

// A helper that opens a change stream on the whole cluster with the user supplied match expression
// 'userMatchExpr' and validates that:
// 1. for each shard, the events are seen in that order as specified in 'expectedResult'
// 2. the filtering is been done at oplog level
function verifyOnWholeCluster(
    resumeAfterToken, userMatchExpr, expectedResult, expectedOplogRetDocsForEachShard) {
    verifyChangeStreamOnWholeCluster({
        st: st,
        changeStreamSpec: {resumeAfter: resumeAfterToken},
        userMatchExpr: userMatchExpr,
        expectedResult: expectedResult,
        expectedOplogNReturnedPerShard:
            [expectedOplogRetDocsForEachShard, expectedOplogRetDocsForEachShard]
    });
}

// Enable a failpoint that will prevent $expr match expressions from generating $_internalExprEq
// or similar expressions. This ensures that the following test-cases only exercise the $expr
// rewrites.
assert.commandWorked(
    db.adminCommand({configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));
FixtureHelpers.runCommandOnEachPrimary({
    db: db.getSiblingDB("admin"),
    cmdObj: {configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}
});

// Create some new collections to ensure that test cases has sufficient namespaces to verify
// that the namespace filtering is working correctly.
const coll2 = createShardedCollection(st, "_id" /* shardKey */, dbName, "coll2", 4 /* splitAt */);
const coll3 =
    createShardedCollection(st, "_id" /* shardKey */, dbName, "coll.coll3", 6 /* splitAt */);
const coll4 = createShardedCollection(st, "_id" /* shardKey */, dbName, "coll4", 10 /* splitAt */);

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken =
    db.getSiblingDB("admin").watch([], {allChangesForCluster: true}).getResumeToken();

// For each collection, insert 2 documents, one on each shard. These will create oplog events and
// change stream will apply various namespace filtering on these collections to verify that the
// namespace is rewritten correctly. Each documents also contains field names matching with that of
// '$cmd' operations, ie. 'renameCollection', 'drop' and 'dropDatabase', but with value-type other
// than strings. The 'ns' match filters should gracefully handle the type mismatch and not throw any
// error.
//
// Each of these inserted documents will be represented in this form in the oplog:
//   {... "o": {"_id": <id>, "renameCollection": true, "drop": {}, "dropDatabase": null}, ...}
//
// A few collections are renamed and dropped to verify that these are filtered properly.
assert.commandWorked(coll.insert({_id: 1, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(coll.insert({_id: 2, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(coll2.insert({_id: 3, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(coll2.insert({_id: 4, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(coll2.renameCollection("newColl2"));
assert.commandWorked(coll3.insert({_id: 5, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(coll3.insert({_id: 6, renameCollection: true, drop: {}, dropDatabase: null}));
assert(coll3.drop());

// Insert some documents into 'coll4' with field names which match known command types. Despite the
// fact that these documents could potentially match with the partial 'ns' filter we rewrite into
// the oplog, the {op: "c"} predicate we add into the filter should ensure that they are correctly
// discarded.
assert.commandWorked(coll4.insert(
    {_id: 7, renameCollection: coll2.getName(), drop: coll3.getName(), dropDatabase: 1}));
assert.commandWorked(coll4.insert({_id: 8, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(
    coll4.insert({_id: 9, renameCollection: "no_dot_ns", drop: "", dropDatabase: ""}));
assert.commandWorked(coll4.insert(
    {_id: 10, renameCollection: coll2.getName(), drop: coll3.getName(), dropDatabase: 1}));
assert.commandWorked(coll4.insert({_id: 11, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(
    coll4.insert({_id: 12, renameCollection: "no_dot_ns", drop: "", dropDatabase: ""}));

// These events from unmonitored collection should not been seen unexpectedly.
assert.commandWorked(
    otherColl.insert({_id: 1, renameCollection: true, drop: {}, dropDatabase: null}));
assert.commandWorked(
    otherColl.insert({_id: 2, renameCollection: true, drop: {}, dropDatabase: null}));

// This group of tests ensures that the '$match' on a particular namespace object only sees its
// documents and only required document(s) are returned at the oplog for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {db: dbName, coll: "coll1"}}},
                     {coll1: {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: "coll1"}]}}},
                     {coll1: {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {db: dbName, coll: "coll2"}}},
                     {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: "coll2"}]}}},
                     {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {db: dbName, coll: "coll.coll3"}}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: "coll.coll3"}]}}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the namespace with only db component should not emit any document and
// the oplog should not return any documents.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {ns: {db: dbName}}}, {}, 0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName}]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the namespace object with 'unknown' collection does not exists and the oplog cursor
// returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {db: dbName, coll: "unknown"}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: "unknown"}]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the namespace object with flipped fields does not match with the namespace object and
// the oplog cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {coll: "coll1", db: dbName}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {coll: "coll1", db: dbName}]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the namespace object with extra fields does not match with the namespace object and
// the oplog cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {db: dbName, coll: "coll1", extra: "extra"}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: "unknown", extra: "extra"}]}}},
    {},
    0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the empty namespace object does not match with the namespace object and the oplog
// cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken, {$match: {ns: {}}}, {}, 0);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {}]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure the '$match' on namespace's db should return documents for all collection and oplog should
// return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": dbName}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                     },
                     8 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db", dbName]}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                     },
                     8 /* expectedOplogRetDocsForEachShard */);

// These cases ensure that the '$match' on regex of namespace' db, should return documents for all
// collection and oplog should return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": /^change_stream_match_pushdown.*$/}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                     },
                     8 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$ns.db", regex: "^change_stream_match_pushdown.*$"}}}},
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
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match:
            {$expr: {$regexMatch: {input: "$ns.db", regex: "(^change_stream_match_pushdown.*$)"}}}
    },
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
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $regexMatch:
                    {input: "$ns.db", regex: "^(Change_Stream_MATCH_PUSHDOWN.*$)", options: "i"}
            }
        }
    },
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
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $regexMatch:
                    {input: "$ns.db", regex: "(^unknown$|^change_stream_match_pushdown.*$)"}
            }
        }
    },
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
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $regexMatch: {input: "$ns.db", regex: "^unknown$|^change_stream_match_pushdown.*$"}
            }
        }
    },
    {
        coll1: {insert: [1, 2]},
        coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
        "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
        "coll4": {insert: [7, 8, 9, 10, 11, 12]}
    },
    8 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on non-existing db should not return any document and oplog should not
// return any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"ns.db": "unknown"}}, {}, 0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db", "unknown"]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on empty db should not return any document and oplog should not return
// any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"ns.db": ""}}, {}, 0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db", ""]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on sub field of db should not return any document and oplog should not
// return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db.extra": dbName}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db.extra", "unknown"]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// This group of tests ensures that the '$match' on collection field path should emit only the
// required documents and oplog should return only required document(s) for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": "coll1"}},
                     {coll1: {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll", "coll1"]}}},
                     {coll1: {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": "coll2"}},
                     {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll", "coll2"]}}},
                     {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": "coll.coll3"}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll", "coll.coll3"]}}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);

// This group of tests ensures that the '$match' on the regex of the collection field path should
// emit only the required documents and oplog should return only required document(s) for each
// shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": /^col.*1/}},
                     {coll1: {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^col.*1"}}}},
                     {coll1: {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": /^col.*2/}},
                     {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^col.*2"}}}},
                     {coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": /^col.*3/}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^col.*3"}}}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);

// This group of tests ensures that the '$match' on the regex matching all collections should return
// documents from all collection and oplog should return all document for each shard.
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
                     {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^col.*"}}}},
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
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^CoLL.*", options: "i"}}}},
    {
        coll1: {insert: [1, 2]},
        coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
        "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
        "coll4": {insert: [7, 8, 9, 10, 11, 12]}
    },
    8 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the regex matching 3 collection should return documents from these
// collections and oplog should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": /^col.*1$|^col.*2$|^col.*3$/}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}
                     },
                     5 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^col.*1$|^col.*2$|^col.*3$"}}}},
    {
        coll1: {insert: [1, 2]},
        coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
        "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}
    },
    5 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the regex to exclude 'coll1', 'coll2' and 'coll4' should return only
// documents from 'coll.coll3' and oplog should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": /^coll[^124]/}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^coll[^124]"}}}},
                     {"coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]}},
                     2 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on non-existing collection should not return any document and oplog
// should not return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": "unknown"}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll", "unknown"]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on empty collection should not return any document and oplog should not
// return any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"ns.coll": ""}}, {}, 0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll", ""]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on sub field of collection should not return any document and oplog
// should not return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll.extra": "coll1"}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll.extra", "coll1"]}}},
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
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$ns.db", [dbName]]}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                     },
                     8 /* expectedOplogRetDocsForEachShard */);

// This group of tests ensures that '$in' and equivalent '$expr' expression on regex matching the db
// name should return all documents and oplog should return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$in: [/^change_stream_match.*$/]}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                     },
                     8 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$or: [{$regexMatch: {input: "$ns.db", regex: "^change_stream_match.*$"}}]}}},
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
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$regexMatch: {input: "$ns.db", regex: "^change_stream_MATCH.*$", options: "i"}}
                ]
            }
        }
    },
    {
        coll1: {insert: [1, 2]},
        coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
        "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
        "coll4": {insert: [7, 8, 9, 10, 11, 12]}
    },
    8 /* expectedOplogRetDocsForEachShard */);

// Ensure that an empty '$in' on db path should not match any collection and oplog should not return
// any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"ns.db": {$in: []}}}, {}, 0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$ns.db", []]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' with invalid db cannot be rewritten and oplog should return all documents for
// each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$in: [dbName, 1]}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]}
                     },
                     9 /* expectedOplogRetDocsForEachShard */);

// Ensure tht '$expr' with mix of valid and invalid db names should return required documents at the
// oplog for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$ns.db", [dbName, 1]]}}},
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
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$eq: ["$ns.db", "unknown1"]},
                                     {$regexMatch: {input: "$ns.db", regex: "^unknown2$"}}
                                 ]
                             }
                         }
                     },
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' on multiple collections should return the required documents and oplog should
// return required documents for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"ns": {$in: [{db: dbName, coll: "coll1"}, {db: dbName, coll: "coll2"}]}}},
    {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
    3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$in: ["$ns", [{db: dbName, coll: "coll1"}, {db: dbName, coll: "coll2"}]]}}},
    {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
    3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"ns.coll": {$in: ["coll1", "coll2"]}}},
    {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
    3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$in: ["$ns.coll", ["coll1", "coll2"]]}}},
    {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
    3 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' on regex of multiple collections should return the required documents and oplog
// should return required documents for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"ns.coll": {$in: [/^coll1$/, /^coll2$/]}}},
    {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
    3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$regexMatch: {input: "$ns.coll", regex: "^coll1$"}},
                    {$regexMatch: {input: "$ns.coll", regex: "^coll2$"}}
                ]
            }
        }
    },
    {coll1: {insert: [1, 2]}, coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]}},
    3 /* expectedOplogRetDocsForEachShard */);

// This group of tests ensures that '$in' and equivalent '$expr' expression on regex of matching all
// collections should return all documents and oplog should return all documents for each shard.
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
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$regexMatch: {input: "$ns.coll", regex: "^coll.*$"}},
                                 ]
                             }
                         }
                     },
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
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$regexMatch: {input: "$ns.coll", regex: "^COLL.*$", options: "i"}},
                ]
            }
        }
    },
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
                     9 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$expr' on '$in' with mix of valid and invalid collections should return only
// required documents at oplog for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$ns.coll", ["coll1", 1]]}}},
                     {coll1: {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' with mix of string and regex matching collections can be rewritten and oplog
// should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$in: ["coll1", /^coll.*3$/]}}},
                     {
                         coll1: {insert: [1, 2]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                     },
                     3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$eq: ["$ns.coll", "coll1"]},
                                     {$regexMatch: {input: "$ns.coll", regex: "^coll.*3$"}},
                                 ]
                             }
                         }
                     },
                     {
                         coll1: {insert: [1, 2]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                     },
                     3 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' and equivalent '$expr' expression with mix of string and regex can be rewritten
// and oplog should return '0' document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$in: ["unknown1", /^unknown2$/]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$eq: ["$ns.coll", "unknown1"]},
                                     {$regexMatch: {input: "$ns.coll", regex: "^unknown2$"}},
                                 ]
                             }
                         }
                     },
                     {},
                     0 /* expectedOplogRetDocsForEachShard */);

// This group of tests ensure that '$nin' and equivalent '$expr' expression on db path should return
// all documents and oplog should return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$nin: []}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]},
                         "other_coll": {insert: [1, 2]}
                     },
                     9 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$nin: ["unknown"]}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]},
                         "other_coll": {insert: [1, 2]}
                     },
                     9 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$or: [{$eq: ["$ns.db", "unknown"]}]}}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]},
                         "other_coll": {insert: [1, 2]}
                     },
                     9 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$nin: [/^unknown$/]}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]},
                         "other_coll": {insert: [1, 2]}
                     },
                     9 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$not: {$or: [{$regexMatch: {input: "$ns.db", regex: "^unknown$"}}]}}}},
    {
        coll1: {insert: [1, 2]},
        coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
        "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
        "coll4": {insert: [7, 8, 9, 10, 11, 12]},
        "other_coll": {insert: [1, 2]}
    },
    9 /* expectedOplogRetDocsForEachShard */);

// These group of tests ensure that '$nin' and equivalent '$expr' expression on matching db name
// should only return documents from unmonitored db and oplog should return only required documents
// from unmonitored db.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$nin: [dbName]}}},
                     {"other_coll": {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$or: [{$eq: ["$ns.db", dbName]}]}}}},
                     {"other_coll": {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$nin: [/change_stream_match_pushdown_and_rewr.*/]}}},
                     {"other_coll": {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $not: {
                    $or: [{
                        $regexMatch:
                            {input: "$ns.db", regex: "change_stream_match_pushdown_and_rewr.*"}
                    }]
                }
            }
        }
    },
    {"other_coll": {insert: [1, 2]}},
    1 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$nin' and equivalent '$expr' expression on multiple collections should return the
// required documents and oplog should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$nin: ["coll1", "coll2", "coll4"]}}},
                     {
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "other_coll": {insert: [1, 2]}
                     },
                     3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$in: ["$ns.coll", ["coll1", "coll2", "coll4"]]}}}},
                     {
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "other_coll": {insert: [1, 2]}
                     },
                     3 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$nin' and equivalent '$expr' expression on regex of multiple collections should
// return the required documents and oplog should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$nin: [/^coll1$/, /^coll2$/, /^coll4$/]}}},
                     {
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "other_coll": {insert: [1, 2]}
                     },
                     3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$regexMatch: {input: "$ns.coll", regex: "^coll1$"}},
                                         {$regexMatch: {input: "$ns.coll", regex: "^coll2$"}},
                                         {$regexMatch: {input: "$ns.coll", regex: "^coll4$"}}
                                     ]
                                 }
                             }
                         }
                     },
                     {
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "other_coll": {insert: [1, 2]}
                     },
                     3 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$nin' and equivalent '$expr' expression on regex of matching all collections should
// return documents from unmonitored db and oplog should also return documentss for unmonitored db
// each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$nin: [/^coll.*$/, /^sys.*$/]}}},
                     {"other_coll": {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$regexMatch: {input: "$ns.coll", regex: "^coll.*$"}},
                                         {$regexMatch: {input: "$ns.coll", regex: "^sys.*$"}}
                                     ]
                                 }
                             }
                         }
                     },
                     {"other_coll": {insert: [1, 2]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that an empty '$nin' and equivalent '$expr' expression should match all collections and
// oplog should return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$nin: []}}},
                     {
                         coll1: {insert: [1, 2]},
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "coll4": {insert: [7, 8, 9, 10, 11, 12]},
                         "other_coll": {insert: [1, 2]}
                     },
                     9 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$nin' with invalid collection cannot be rewritten and oplog should return all
// documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$nin: ["coll1", 1]}}},
                     {
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         coll4: {insert: [7, 8, 9, 10, 11, 12]},
                         "other_coll": {insert: [1, 2]}
                     },
                     9 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$expr' with mix of valid and invalid collection should return required documents at
// the oplog for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$in: ["$ns.coll", ["coll1", 1]]}}}},
                     {
                         coll2: {insert: [3, 4], rename: ["newColl2", "newColl2"]},
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         coll4: {insert: [7, 8, 9, 10, 11, 12]},
                         "other_coll": {insert: [1, 2]}
                     },
                     8 /* expectedOplogRetDocsForEachShard */);

// Ensure that '$nin' and equivalent '$expr' expression with mix of string and regex can be
// rewritten and oplog should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$nin: ["coll1", /^coll2$/, "coll4"]}}},
                     {
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "other_coll": {insert: [1, 2]}
                     },
                     3 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$in: ["$ns.coll", ["coll1", "coll4"]]},
                                         {$regexMatch: {input: "$ns.coll", regex: "^coll2$"}},
                                     ]
                                 }
                             }
                         }
                     },
                     {
                         "coll.coll3": {insert: [5, 6], drop: ["coll.coll3", "coll.coll3"]},
                         "other_coll": {insert: [1, 2]}
                     },
                     3 /* expectedOplogRetDocsForEachShard */);

// At this stage, the coll2 has been renamed to 'newColl2' and coll3 has been dropped. The test from
// here will drop the database and ensure that the 'ns' filter when applied over the collection
// should only emit the 'drop' event for that collection and not the 'dropDatabase' event. It should
// be noted that for 'newColl2' and 'coll3', the 'dropDatabase' will be no-op and will not emit any
// documents.

// Open a new change streams and verify that from here onwards the events related to 'dropDatabase'
// are seen.
const secondResumeAfterToken =
    db.getSiblingDB("admin").watch([], {allChangesForCluster: true}).getResumeToken();

assert.commandWorked(db.dropDatabase());

// This group of tests ensures that the match on 'coll1' only sees the 'drop' events.
verifyOnWholeCluster(secondResumeAfterToken,
                     {$match: {ns: {db: dbName, coll: "coll1"}}},
                     {coll1: {drop: ["coll1", "coll1"]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(secondResumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: "coll1"}]}}},
                     {coll1: {drop: ["coll1", "coll1"]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(secondResumeAfterToken,
                     {$match: {"ns.coll": "coll1"}},
                     {coll1: {drop: ["coll1", "coll1"]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(secondResumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll", "coll1"]}}},
                     {coll1: {drop: ["coll1", "coll1"]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(secondResumeAfterToken,
                     {$match: {"ns.coll": /^col.*1/}},
                     {coll1: {drop: ["coll1", "coll1"]}},
                     1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(secondResumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$ns.coll", regex: "^col.*1"}}}},
                     {coll1: {drop: ["coll1", "coll1"]}},
                     1 /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$ns' object containing only 'db' should see only the 'dropDatabase' event and
// only the required documents gets returned at the oplog for each shard.
verifyOnWholeCluster(
    secondResumeAfterToken,
    {$match: {ns: {db: dbName}}},
    {change_stream_match_pushdown_and_rewrite_and_rewrite: {dropDatabase: [dbName, dbName]}},
    1 /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    secondResumeAfterToken,
    {$match: {$expr: {$eq: ["$ns", {db: dbName}]}}},
    {change_stream_match_pushdown_and_rewrite_and_rewrite: {dropDatabase: [dbName, dbName]}},
    1 /* expectedOplogRetDocsForEachShard */);

st.stop();
})();
