// Test that a pipeline of the form [{$changeStream: {}}, {$match: ...}] can rewrite the 'to' and
// apply it to oplog-format documents in order to filter out results as early as possible.
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

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

// A helper that opens a change stream on the whole cluster with the user supplied match expression
// 'userMatchExpr' and validates that:
// 1. for each shard, the events are seen in that order as specified in 'expectedResult'
// 2. the filtering is been done at oplog level
function verifyOnWholeCluster(
    resumeAfterToken, userMatchExpr, expectedResult, expectedOplogRetDocsForEachShard) {
    const cursor = db.getSiblingDB("admin").aggregate([
        {$changeStream: {resumeAfter: resumeAfterToken, allChangesForCluster: true}},
        userMatchExpr
    ]);

    for (const [coll, opDict] of Object.entries(expectedResult)) {
        for (const [op, eventIdentifierList] of Object.entries(opDict)) {
            eventIdentifierList.forEach(eventIdentifier => {
                assert.soon(() => cursor.hasNext());
                const event = cursor.next();
                assert.eq(event.operationType, op, event);

                if (op == "insert") {
                    assert.eq(event.documentKey._id, eventIdentifier, event);
                } else if (op == "rename") {
                    assert.eq(event.to.coll, eventIdentifier, event);
                } else {
                    assert(false, event);
                }
                assert.eq(event.ns.coll, coll);
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

    assertNumMatchingOplogEventsForShard(stats, st.rs0.name, expectedOplogRetDocsForEachShard);
    assertNumMatchingOplogEventsForShard(stats, st.rs1.name, expectedOplogRetDocsForEachShard);
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

// Create some new collections to ensure that test cases has sufficient namespaces to verify that
// the filtering on the 'to' field is working correctly.
const coll1 = createShardedCollection(st, "_id" /* shardKey */, dbName, "coll1", 5 /* splitAt */);
const coll2 = createShardedCollection(st, "_id" /* shardKey */, dbName, "coll2", 9 /* splitAt */);

const ns1 = dbName + ".newColl1";
const ns2 = dbName + ".newColl2";

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken =
    db.getSiblingDB("admin").watch([], {allChangesForCluster: true}).getResumeToken();

// The inserted documents purposely contain field name 'o.to' to match with that of oplog's 'to'
// field. These fields should not interfere with the rewritten predicate and only expected documents
// should be returned at the oplog level.
assert.commandWorked(coll1.insert({_id: 3, "o.to": 3}));
assert.commandWorked(coll1.insert({_id: 4, "o.to": ""}));
assert.commandWorked(coll1.insert({_id: 5, "o.to": ns2}));
assert.commandWorked(coll1.insert({_id: 6, "o.to": dbName}));
assert.commandWorked(coll1.renameCollection("newColl1"));
assert.commandWorked(coll2.insert({_id: 7, "o.to": 3}));
assert.commandWorked(coll2.insert({_id: 8, "o.to": ""}));
assert.commandWorked(coll2.insert({_id: 9, "o.to": ns1}));
assert.commandWorked(coll2.insert({_id: 10, "o.to": dbName}));
assert.commandWorked(coll2.renameCollection("newColl2"));

// This group of tests ensures that the '$match' on the 'to' object and equivalent '$expr'
// expression only sees its documents and only required document(s) are returned at the oplog for
// each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {to: {db: dbName, coll: "newColl1"}}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to", {db: dbName, coll: "newColl1"}]}}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {to: {db: dbName, coll: "newColl2"}}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to", {db: dbName, coll: "newColl2"}]}}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on the 'to' object and equivalent '$expr' expression with only db
// component should not emit any document and the oplog should not return any documents.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {to: {db: dbName}}}, {}, 0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to", {db: dbName}]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the 'to' object and equivalent '$expr' expression with 'unknown' collection does not
// exists and the oplog cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {to: {db: dbName, coll: "unknown"}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to", {db: dbName, coll: "unknown"}]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the 'to' object and equivalent '$expr' expression with flipped fields does not match
// and the oplog cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {to: {coll: "newColl1", db: dbName}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to", {coll: "newColl1", db: dbName}]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the 'to' object and equivalent '$expr' expression with extra fields does not match
// and the oplog cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {to: {db: dbName, coll: "newColl1", extra: "extra"}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$eq: ["$to", {db: dbName, coll: "newColl1", extra: "extra"}]}}},
    {},
    0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the empty 'to' object does not match and the oplog cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken, {$match: {to: {}}}, {}, 0);

// Ensure the '$match' on the db field path and equivalent '$expr' expression should only return
// documents with rename op type for all collections and oplog should also return same for each
// shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": dbName}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$eq: ["$to.db", dbName]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": /^change_stream_match_pushdown.*$/}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$to.db", regex: "^change_stream_match_pushdown.*$"}}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": /^(change_stream_match_pushdown.*$)/}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match:
            {$expr: {$regexMatch: {input: "$to.db", regex: "^(change_stream_match_pushdown.*$)"}}}
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": /^(Change_Stream_MATCH_PUSHDOWN.*$)/i}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $regexMatch:
                    {input: "$to.db", regex: "^(Change_Stream_MATCH_PUSHDOWN.*$)", options: "i"}
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": /(^unknown$|^change_stream_match_pushdown.*$)/}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $regexMatch:
                    {input: "$to.db", regex: "(^unknown$|^change_stream_match_pushdown.*$)"}
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": /^unknown$|^change_stream_match_pushdown.*$/}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $regexMatch: {input: "$to.db", regex: "^unknown$|^change_stream_match_pushdown.*$"}
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on non-existing db and equivalent '$expr' expression should not return
// any document and oplog should not return any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"to.db": "unknown"}}, {}, 0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.db", "unknown"]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on empty db and equivalent '$expr' expression should not return any
// document and oplog should not return any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"to.db": ""}}, {}, 0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.db", ""]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on sub field of db and equivalent '$expr' expression should not return
// any document and oplog should not return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.db.extra": dbName}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.db.extra", dbName]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// This group of tests ensures that the '$match' on collection field path and equivalent '$expr'
// expression should emit only the required documents and oplog should return only required
// document(s) for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": "newColl1"}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.coll", "newColl1"]}}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": "newColl2"}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.coll", "newColl2"]}}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": /^newColl.*1/}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$to.coll", regex: "^newColl.*1"}}}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": /^newColl.*2/}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$to.coll", regex: "^newColl.*2"}}}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);

// This group of tests ensures that the '$match' on the regex matching all collections and
// equivalent '$expr' expression should return documents with rename op type and oplog should also
// return same for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": /^newColl.*/}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$to.coll", regex: "^newColl.*"}}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": /^newColl.*/i}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$to.coll", regex: "^newColl.*", options: "i"}}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": /^newColl.*1$|^newColl.*2$/}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$to.coll", regex: "^newColl.*1$|^newColl.*2$"}}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on the regex to exclude 'coll1' and equivalent '$expr' expression should
// return only documents from 'coll2' and oplog should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": /^newColl[^1]/}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$regexMatch: {input: "$to.coll", regex: "^newColl[^1]"}}}},
                     {coll2: {rename: ["newColl2", "newColl2"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on non-existing collection and equivalent '$expr' expression should not
// return any document and oplog should not return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": "unknown"}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.coll", "unknown"]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on empty collection and equivalent '$expr' expression should not return
// any document and oplog should not return any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"to.coll": ""}}, {}, 0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.coll", ""]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that the '$match' on sub field of collection and equivalent '$expr' expression should not
// return any document and oplog should not return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll.extra": "coll1"}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$to.coll.extra", "coll1"]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$in' on db and equivalent '$expr' expression should return all documents with rename
// op type and oplog should also return same each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": {$in: [dbName, "unknown"]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$in: ["$to.db", [dbName, "unknown"]]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": {$in: [/^change_stream_match.*$/, /^unknown$/]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$regexMatch: {input: "$to.db", regex: "^change_stream_match.*$"}},
                    {$regexMatch: {input: "$to.db", regex: "^unknown$"}}
                ]
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": {$in: [/^change_stream_MATCH.*$/i, /^unknown$/i]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {
                        $regexMatch:
                            {input: "$to.db", regex: "^change_stream_MATCH.*$", options: "i"}
                    },
                    {$regexMatch: {input: "$to.db", regex: "^unknown$", options: "i"}}
                ]
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);

// Ensure that an empty '$in' on db path and equivalent '$expr' expression should not match any
// collection and oplog should not return any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"to.db": {$in: []}}}, {}, 0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$to.db", []]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$in' with invalid db cannot be rewritten and oplog should return all documents for
// each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": {$in: [dbName, 1]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    6 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$expr' with '$in' with one valid and one invalid db should return required documents
// at oplog for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$in: ["$to.db", [dbName, 1]]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$in' on db path with mix of string and regex and equivalent '$expr' expression can
// be rewritten and oplog should return '0' document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.db": {$in: ["unknown1", /^unknown2$/]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$eq: ["$to.db", "unknown1"]},
                                     {$regexMatch: {input: "$to.db", regex: "^unknown2$"}}
                                 ]
                             }
                         }
                     },
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$in' on multiple collections and equivalent '$expr' expression should return the
// required documents and oplog should return required documents for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to": {$in: [{db: dbName, coll: "newColl1"}, {db: dbName, coll: "newColl2"}]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {$in: ["$to", [{db: dbName, coll: "newColl1"}, {db: dbName, coll: "newColl2"}]]}
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": {$in: ["newColl1", "newColl2"]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$in: ["$to.coll", ["newColl1", "newColl2"]]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": {$in: [/^newColl1$/, /^newColl2$/]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$regexMatch: {input: "$to.coll", regex: "^newColl1$"}},
                    {$regexMatch: {input: "$to.coll", regex: "^newColl2$"}}
                ]
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);

// This group of tests ensures that '$in' on regex of matching all collections and equivalent
// '$expr' expression should return all documents with rename op type and oplog should also return
// same for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": {$in: [/^newColl.*$/, /^unknown.*/]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$regexMatch: {input: "$to.coll", regex: "^newColl.*$"}},
                    {$regexMatch: {input: "$to.coll", regex: "^unknown.*"}}
                ]
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": {$in: [/^newcoll.*$/i, /^unknown/i]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$regexMatch: {input: "$to.coll", regex: "^newcoll.*$", options: "i"}},
                    {$regexMatch: {input: "$to.coll", regex: "^unknown", options: "i"}}
                ]
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);

// Ensure that an empty '$in' should not match any collection and oplog should not return any
// document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$in: []}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$in' with invalid collection cannot be rewritten and oplog should return all
// documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$in: ["newColl1", 1]}}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     6 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$expr' with '$in' with one valid and one invalid collection should return only
// required documents at the oplog for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$to.coll", ["newColl1", 1]]}}},
                     {coll1: {rename: ["newColl1", "newColl1"]}},
                     1 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$in' with mix of string and regex matching collections and equivalent '$expr'
// expression can be rewritten and oplog should return required documents for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.coll": {$in: ["newColl1", /^newColl.*2$/]}}},
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $or: [
                    {$eq: ["$to.coll", "newColl1"]},
                    {$regexMatch: {input: "$to.coll", regex: "^newColl.*2"}}
                ]
            }
        }
    },
    {coll1: {rename: ["newColl1", "newColl1"]}, coll2: {rename: ["newColl2", "newColl2"]}},
    2 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$in' with mix of string and regex and equivalent '$expr' expression can be rewritten
// and oplog should return '0' document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$in: ["unknown1", /^unknown2$/]}}},
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$eq: ["$to.coll", "unknown1"]},
                                     {$regexMatch: {input: "$to.coll", regex: "^unknown2$"}}
                                 ]
                             }
                         }
                     },
                     {},
                     0 /* expectedOplogRetDocsForEachShard*/);

// This group of tests ensure that '$nin' on db path and equivalent '$expr' expression should return
// all documents and oplog should return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.db": {$nin: []}}},
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$in: ["$to.db", []]}}}},
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.db": {$nin: ["unknown1", "unknown2"]}}},
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$in: ['$to.db', ["unknown1", "unknown2"]]}}}},
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.db": {$nin: [/^unknown1$/, /^unknown2$/]}}},
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$regexMatch: {input: "$to.db", regex: "^unknown1$"}},
                                         {$regexMatch: {input: "$to.db", regex: "^unknown2$"}}
                                     ]
                                 }
                             }
                         }
                     },
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);

// These group of tests ensure that '$nin' on matching db name and equivalent '$expr' expression
// should not return any documents with rename op type and oplog should also return same each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.db": {$nin: [dbName, "unknown"]}}},
                     {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
                     4 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$in: ["$to.db", [dbName, "unknown"]]}}}},
                     {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
                     4 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"to.db": {$nin: [/change_stream_match_pushdown_and_rewr.*/, /^unknown.*/]}}},
    {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
    4 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $not: {
                    $or: [
                        {
                            $regexMatch:
                                {input: "$to.db", regex: "change_stream_match_pushdown_and_rewr.*"}
                        },
                        {$regexMatch: {input: "$to.db", regex: "^unknown.*"}}
                    ]
                }
            }
        }
    },
    {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
    4 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$nin' on a collection and equivalent '$expr' expression should return the documents
// with only insert op type for that collection and oplog should also return same for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$nin: ["newColl1", "unknown"]}}},
                     {
                         coll1: {insert: [3, 4, 5, 6]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     5 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$not: {$or: [{$in: ["$to.coll", ["newColl1", "unknown"]]}]}}}},
    {
        coll1: {insert: [3, 4, 5, 6]},
        coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
    },
    5 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$nin: [/^newColl1$/, /^unknown$/]}}},
                     {
                         coll1: {insert: [3, 4, 5, 6]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     5 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$regexMatch: {input: "$to.coll", regex: "^newColl1$"}},
                                         {$regexMatch: {input: "$to.coll", regex: "^unknown$"}}
                                     ]
                                 }
                             }
                         }
                     },
                     {
                         coll1: {insert: [3, 4, 5, 6]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     5 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$nin' on regex of matching all collections and equivalent '$expr' expression should
// only return documents with op type insert and oplog should also return same for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$nin: [/^newColl.*$/, /^unknown.*$/]}}},
                     {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
                     4 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$regexMatch: {input: "$to.coll", regex: "^newColl.*$"}},
                                         {$regexMatch: {input: "$to.coll", regex: "^unknown.*$"}}
                                     ]
                                 }
                             }
                         }
                     },
                     {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
                     4 /* expectedOplogRetDocsForEachShard*/);

// Ensure that an empty '$nin' should match all collections and oplog should return all documents
// for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$nin: []}}},
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$in: ["$to.coll", []]}}}},
                     {
                         coll1: {insert: [3, 4, 5, 6], rename: ["newColl1", "newColl1"]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$nin' with invalid collection cannot be rewritten and oplog should return all
// documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$nin: ["newColl1", 1]}}},
                     {
                         coll1: {insert: [3, 4, 5, 6]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     6 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$expr' with '$nin' with one valid and one invalid collection should return required
// documents at the oplog for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$not: {$in: ["$to.coll", ["newColl1", 1]]}}}},
                     {
                         coll1: {insert: [3, 4, 5, 6]},
                         coll2: {insert: [7, 8, 9, 10], rename: ["newColl2", "newColl2"]}
                     },
                     5 /* expectedOplogRetDocsForEachShard*/);

// Ensure that '$nin' with mix of string and regex and equivalent '$expr' expression can be
// rewritten and oplog should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"to.coll": {$nin: ["newColl1", /^newColl2$/]}}},
                     {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
                     4 /* expectedOplogRetDocsForEachShard*/);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$eq: ["$to.coll", "newColl1"]},
                                         {$regexMatch: {input: "$to.coll", regex: "^newColl2$"}}
                                     ]
                                 }
                             }
                         }
                     },
                     {coll1: {insert: [3, 4, 5, 6]}, coll2: {insert: [7, 8, 9, 10]}},
                     4 /* expectedOplogRetDocsForEachShard*/);

st.stop();
})();
