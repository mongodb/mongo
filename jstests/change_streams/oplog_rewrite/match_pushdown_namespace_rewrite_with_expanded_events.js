// Test that a pipeline of the form [{$changeStream: {}}, {$match: ...}] can rewrite the 'namespace'
// and apply it to oplog-format documents in order to filter out results as early as possible,
// specifially for the newly added events that are behind the 'showExpandedEvents' flag.
//
// @tags: [
//   requires_fcv_60,
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
const shard0Only = "shard0Only";
const shard1Only = "shard1Only";
const otherDbName = "other_db";
const collName = "coll.coll1";
const coll2Name = "coll1.coll2";
const otherCollName = "coll.coll1.coll2";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;

assert.commandWorked(st.s.adminCommand({enableSharding: shard0Only}));
st.ensurePrimaryShard(shard0Only, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({enableSharding: shard1Only}));
st.ensurePrimaryShard(shard1Only, st.shard1.shardName);

const db = mongosConn.getDB(dbName);

// A helper that opens a change stream on the whole cluster with the user supplied match expression
// 'userMatchExpr' and validates that:
// 1. for each shard, the events are seen in that order as specified in 'expectedResult'
// 2. the filtering is been done at oplog level
function verifyOnWholeCluster(
    resumeAfterToken, userMatchExpr, expectedResult, expectedOplogRetDocsForEachShard) {
    verifyChangeStreamOnWholeCluster({
        st: st,
        changeStreamSpec: {resumeAfter: resumeAfterToken, showExpandedEvents: true},
        userMatchExpr: userMatchExpr,
        expectedResult: expectedResult,
        expectedOplogNReturnedPerShard: expectedOplogRetDocsForEachShard
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

const coll = createShardedCollection(st, "_id" /* shardKey */, dbName, collName, 2 /* splitAt */);

// Create a sharded collection in the "other" database.
const unmoniterdColl =
    createShardedCollection(st, "_id" /* shardKey */, otherDbName, otherCollName, 2 /* splitAt */);

// Create some new collections to ensure that test cases has sufficient namespaces to verify
// that the namespace filtering is working correctly.
const coll2 = createShardedCollection(st, "_id" /* shardKey */, dbName, coll2Name, 2 /* splitAt */);

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken =
    db.getSiblingDB("admin").watch([], {allChangesForCluster: true}).getResumeToken();

// For each collection, do a bunch of write operations, specifically 'create', 'createIndexes',
// 'dropIndexes' and 'collMod' so that we can validate the pushdown optimizations later on.
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.insertMany([{_id: 1}, {_id: 2}]));
assert.commandWorked(coll.dropIndex({x: 1}));

assert.commandWorked(coll2.createIndex({create: 1}));
assert.commandWorked(
    coll2.runCommand({collMod: coll2.getName(), index: {keyPattern: {create: 1}, hidden: true}}));

assert.commandWorked(coll2.insertMany([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));

// These events from unmonitored collection should not been seen unexpectedly.
assert.commandWorked(unmoniterdColl.insertMany([{_id: 1}, {_id: 2}]));

assert.commandWorked(db.getSiblingDB(shard0Only).createCollection(shard0Only));
assert.commandWorked(db.getSiblingDB(shard1Only).createCollection(shard1Only));

// This group of tests ensures that the '$match' on a particular namespace object only sees its
// documents and only required document(s) are returned at the oplog for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {ns: {db: dbName, coll: collName}}},
    {
        [collName]:
            {createIndexes: [collName, collName], insert: [1, 2], dropIndexes: [collName, collName]}
    },
    [3, 3] /* expectedOplogRetDocsForEachShard */);

verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: collName}]}}},
    {
        [collName]:
            {createIndexes: [collName, collName], insert: [1, 2], dropIndexes: [collName, collName]}
    },
    [3, 3] /* expectedOplogRetDocsForEachShard */);

verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: coll2Name}]}}},
                     {
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [4, 4] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on the namespace with only db component should not emit any document and
// the oplog should not return any documents.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {db: dbName}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName}]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that the namespace object with 'unknown' collection does not exists and the oplog cursor
// returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {db: dbName, coll: "unknown"}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: "unknown"}]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that the namespace object with flipped fields does not match with the namespace object and
// the oplog cursor returns 0 document.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {ns: {coll: collName, db: dbName}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {coll: collName, db: dbName}]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that the empty namespace object does not match with the namespace object and the oplog
// cursor returns 0 document.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {ns: {}}}, {}, [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns", {}]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure the '$match' on namespace's db should return documents for all collection and oplog should
// return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": dbName}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db", dbName]}}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);

// These cases ensure that the '$match' on regex of namespace' db, should return documents for all
// collection and oplog should return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": /^change_stream_match_pushdown.*$/}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$regexMatch: {input: "$ns.db", regex: "^change_stream_match_pushdown.*$"}}}},
    {
        [collName]: {
            createIndexes: [collName, collName],
            insert: [1, 2],
            dropIndexes: [collName, collName]
        },
        [coll2Name]: {
            createIndexes: [coll2Name, coll2Name],
            modify: [coll2Name, coll2Name],
            insert: [0, 1, 2, 3]
        }
    },
    [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": /^(change_stream_match_pushdown.*$)/}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match:
            {$expr: {$regexMatch: {input: "$ns.db", regex: "(^change_stream_match_pushdown.*$)"}}}
    },
    {
        [collName]: {
            createIndexes: [collName, collName],
            insert: [1, 2],
            dropIndexes: [collName, collName]
        },
        [coll2Name]: {
            createIndexes: [coll2Name, coll2Name],
            modify: [coll2Name, coll2Name],
            insert: [0, 1, 2, 3]
        }
    },
    [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": /^(Change_Stream_MATCH_PUSHDOWN.*$)/i}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
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
        [collName]: {
            createIndexes: [collName, collName],
            insert: [1, 2],
            dropIndexes: [collName, collName]
        },
        [coll2Name]: {
            createIndexes: [coll2Name, coll2Name],
            modify: [coll2Name, coll2Name],
            insert: [0, 1, 2, 3]
        }
    },
    [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": /(^unknown$|^change_stream_match_pushdown.*$)/}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
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
        [collName]: {
            createIndexes: [collName, collName],
            insert: [1, 2],
            dropIndexes: [collName, collName]
        },
        [coll2Name]: {
            createIndexes: [coll2Name, coll2Name],
            modify: [coll2Name, coll2Name],
            insert: [0, 1, 2, 3]
        }
    },
    [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": /^unknown$|^change_stream_match_pushdown.*$/}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
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
        [collName]: {
            createIndexes: [collName, collName],
            insert: [1, 2],
            dropIndexes: [collName, collName]
        },
        [coll2Name]: {
            createIndexes: [coll2Name, coll2Name],
            modify: [coll2Name, coll2Name],
            insert: [0, 1, 2, 3]
        }
    },
    [7, 7] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on non-existing db should not return any document and oplog should not
// return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": "unknown"}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db", "unknown"]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on empty db should not return any document and oplog should not return
// any document for each shard.
verifyOnWholeCluster(
    resumeAfterToken, {$match: {"ns.db": ""}}, {}, [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db", ""]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on sub field of db should not return any document and oplog should not
// return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db.extra": dbName}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.db.extra", "unknown"]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// This group of tests ensures that the '$match' on collection field path should emit only the
// required documents and oplog should return only required document(s) for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"ns.coll": collName}},
    {
        [collName]:
            {createIndexes: [collName, collName], insert: [1, 2], dropIndexes: [collName, collName]}
    },
    [3, 3] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {$expr: {$eq: ["$ns.coll", collName]}}},
    {
        [collName]:
            {createIndexes: [collName, collName], insert: [1, 2], dropIndexes: [collName, collName]}
    },
    [3, 3] /* expectedOplogRetDocsForEachShard */);

// Ensure that the '$match' on sub field of collection should not return any document and oplog
// should not return any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll.extra": collName}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$eq: ["$ns.coll.extra", collName]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' on db should return all documents and oplog should return all documents for
// each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$in: [dbName]}}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$ns.db", [dbName]]}}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);

// Ensure that an empty '$in' on db path should not match any collection and oplog should not return
// any document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$in: []}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$ns.db", []]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' with invalid db cannot be rewritten and oplog should return all documents for
// each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$in: [dbName, 1]}}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [9, 9] /* expectedOplogRetDocsForEachShard */);

// Ensure that '$expr' with mix of valid and invalid db names should return required documents at
// the oplog for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {$expr: {$in: ["$ns.db", [dbName, 1]]}}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' on db path with mix of string and regex can be rewritten and oplog should
// return '0' document for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$in: ["unknown1", /^unknown2$/]}}},
                     {},
                     [0, 0] /* expectedOplogRetDocsForEachShard */);
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
                     [0, 0] /* expectedOplogRetDocsForEachShard */);

// Ensure that '$in' on regex of multiple collections should return the required documents and oplog
// should return required documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$in: [/^coll.coll1$/, /^coll1.coll2$/]}}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$regexMatch: {input: "$ns.coll", regex: "^coll.coll1$"}},
                                     {$regexMatch: {input: "$ns.coll", regex: "^coll1.coll2$"}}
                                 ]
                             }
                         }
                     },
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         }
                     },
                     [7, 7] /* expectedOplogRetDocsForEachShard */);

// This group of tests ensures that '$in' and equivalent '$expr' expression on regex of matching all
// collections should return all documents and oplog should return all documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$in: [/^coll.*$/, /^shard.*$/]}}},
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         },
                         [otherCollName]: {insert: [1, 2]},
                         [shard0Only]: {create: [shard0Only]},
                         [shard1Only]: {create: [shard1Only]},
                     },
                     [9, 9] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $or: [
                                     {$regexMatch: {input: "$ns.coll", regex: "^coll.*$"}},
                                     {$regexMatch: {input: "$ns.coll", regex: "^shard.*$"}},
                                 ]
                             }
                         }
                     },
                     {
                         [collName]: {
                             createIndexes: [collName, collName],
                             insert: [1, 2],
                             dropIndexes: [collName, collName]
                         },
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         },
                         [otherCollName]: {insert: [1, 2]},
                         [shard0Only]: {create: [shard0Only]},
                         [shard1Only]: {create: [shard1Only]},
                     },
                     [9, 9] /* expectedOplogRetDocsForEachShard */);

// These group of tests ensure that '$nin' and equivalent '$expr' expression on matching db name
// should only return documents from unmonitored db and oplog should return only required documents
// from unmonitored db.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.db": {$nin: [dbName, shard0Only, shard1Only]}}},
                     {[otherCollName]: {insert: [1, 2]}},
                     [1, 1] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(resumeAfterToken,
                     {
                         $match: {
                             $expr: {
                                 $not: {
                                     $or: [
                                         {$eq: ["$ns.db", dbName]},
                                         {$eq: ["$ns.db", shard0Only]},
                                         {$eq: ["$ns.db", shard1Only]}
                                     ]
                                 }
                             }
                         }
                     },
                     {[otherCollName]: {insert: [1, 2]}},
                     [1, 1] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"ns.db": {$nin: [/change_stream_match_pushdown_and_rewr.*/, /shard.*/]}}},
    {[otherCollName]: {insert: [1, 2]}},
    [1, 1] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match: {
            $expr: {
                $not: {
                    $or: [
                        {
                            $regexMatch:
                                {input: "$ns.db", regex: "change_stream_match_pushdown_and_rewr.*"}
                        },
                        {$regexMatch: {input: "$ns.db", regex: "shard.*"}}
                    ]
                }
            }
        }
    },
    {[otherCollName]: {insert: [1, 2]}},
    [1, 1] /* expectedOplogRetDocsForEachShard */);

// Ensure that '$nin' and equivalent '$expr' expression on multiple collections should return the
// required documents and oplog should return required documents for each shard.
verifyOnWholeCluster(
    resumeAfterToken,
    {$match: {"ns.coll": {$nin: [collName, coll2Name, otherCollName, shard0Only]}}},
    {[shard1Only]: {create: [shard1Only]}},
    [0, 1] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(
    resumeAfterToken,
    {
        $match:
            {$expr: {$not: {$in: ["$ns.coll", [collName, coll2Name, otherCollName, shard0Only]]}}}
    },
    {[shard1Only]: {create: [shard1Only]}},
    [0, 1] /* expectedOplogRetDocsForEachShard */);

// Ensure that '$nin' with invalid collection cannot be rewritten and oplog should return all
// documents for each shard.
verifyOnWholeCluster(resumeAfterToken,
                     {$match: {"ns.coll": {$nin: [collName, 1]}}},
                     {
                         [coll2Name]: {
                             createIndexes: [coll2Name, coll2Name],
                             modify: [coll2Name, coll2Name],
                             insert: [0, 1, 2, 3]
                         },
                         [otherCollName]: {insert: [1, 2]},
                         [shard0Only]: {create: [shard0Only]},
                         [shard1Only]: {create: [shard1Only]},
                     },
                     [9, 9] /* expectedOplogRetDocsForEachShard */);

//
// The below tests are special cases where the pushdown optimizations are not enabled.
//
// The sharding operations are generally logged as no-op operations in the oplog and are only
// emitted by the primary shard of the db. The pushdown optimizations are not enabled for no-op
// operations. Similarly, operations on views also do not make use of any optimizations.
//
db.dropDatabase();
db.getSiblingDB(otherDbName).dropDatabase();

st.adminCommand({enablesharding: dbName});
st.ensurePrimaryShard(dbName, st.shard0.shardName);

st.adminCommand({enablesharding: otherDbName});
st.ensurePrimaryShard(otherDbName, st.shard1.shardName);

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const secondResumeToken =
    db.getSiblingDB("admin").watch([], {allChangesForCluster: true}).getResumeToken();

// The shardCollection command may produce an additional no-op oplog entries like
// 'migrateChunkToNewShard' which is always read by the change stream. So each of the 'shardColl'
// would add two no-op oplog entries, in addition to the 'create' operation.
st.shardColl(collName,
             {_id: 1} /* shard key */,
             {_id: 1} /* split at */,
             {
                 _id: 1
             }, /* move the chunk containing {shardKey: 1} to its own shard. This should result in
                  'migrateChunkToNewShard' oplog entry */
             dbName,
             true);

st.shardColl(coll2Name,
             {_id: 1} /* shard key */,
             {_id: 1} /* split at */,
             {
                 _id: 1
             }, /* move the chunk containing {shardKey: 1} to its own shard. This should result in
                  'migrateChunkToNewShard' oplog entry */
             dbName,
             true);

st.shardColl(otherCollName,
             {_id: 1} /* shard key */,
             {_id: 1} /* split at */,
             {
                 _id: 1
             }, /* move the chunk containing {shardKey: 1} to its own shard. This should result in
                  'migrateChunkToNewShard' oplog entry */
             otherDbName,
             true);

// Operations on views are treated as a special case and the pushdown optimizations does not work on
// the views. So all the below 4 operations should be returned from the oplog scan.
assert.commandWorked(db.createView("view1", coll2Name, [{$project: {a: 1}}]));
assert.commandWorked(db.runCommand({collMod: "view1", viewOn: coll2Name, pipeline: []}));

assert.commandWorked(db.createView("view2", coll2Name, [{$project: {a: 1}}]));
assert.commandWorked(db.runCommand({drop: "view2"}));

// Ensure that the '$match' on the namespace with only db component should not emit any document. We
// should always see the 8 documents (4 from views + 2 for each shardCollection + 2 for each
// migrateChunkToNewShard) on shard0, and 2 documents on shard1. The 'create' operations that do
// undergo pushdown optimization and should only be returned when the match filter matches.
verifyOnWholeCluster(secondResumeToken,
                     {$match: {ns: {db: dbName}}},
                     {},
                     [8, 2] /* expectedOplogRetDocsForEachShard */);
verifyOnWholeCluster(secondResumeToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName}]}}},
                     {},
                     [8, 2] /* expectedOplogRetDocsForEachShard */);

verifyOnWholeCluster(secondResumeToken,
                     {$match: {ns: {db: dbName, coll: collName}}},
                     {[collName]: {create: [collName], shardCollection: [collName]}},
                     [9, 2] /* expectedOplogRetDocsForEachShard */);

verifyOnWholeCluster(secondResumeToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: collName}]}}},
                     {[collName]: {create: [collName], shardCollection: [collName]}},
                     [9, 2] /* expectedOplogRetDocsForEachShard */);

verifyOnWholeCluster(secondResumeToken,
                     {$match: {$expr: {$eq: ["$ns", {db: dbName, coll: coll2Name}]}}},
                     {[coll2Name]: {create: [coll2Name], shardCollection: [coll2Name]}},
                     [9, 2] /* expectedOplogRetDocsForEachShard */);

verifyOnWholeCluster(
    secondResumeToken,
    {$match: {$expr: {$not: {$in: ["$ns.coll", [collName]]}}}},
    {
        [coll2Name]: {create: [coll2Name], shardCollection: [coll2Name]},
        [otherCollName]: {create: [otherCollName], shardCollection: [otherCollName]},
        "view1": {create: ["view1"], modify: ["view1"]},
        "view2": {create: ["view2"], drop: ["view2"]}
    },
    [9, 3] /* expectedOplogRetDocsForEachShard */);

st.stop();
})();
