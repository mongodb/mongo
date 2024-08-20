/**
 * Test that mongos is collecting query stats metrics for agg queries.
 * @tags: [requires_fcv_72]
 */

import {
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats,
    getQueryStatsAggCmd
} from "jstests/libs/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryStatsRateLimit: -1,
            'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
        }
    },
});
const mongos = st.s;
const db = mongos.getDB("test");

// Assert that, for agg queries, no query stats results are written until a cursor has reached
// exhaustion; ensure accurate results once they're written.
{
    const coll = db.coll;
    coll.insert({v: 1});
    coll.insert({v: 4});
    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            command: "aggregate",
            pipeline: [
                {$match: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}},
                {$project: {_id: true, hello: true}}
            ]

        },
        cursor: {batchSize: "?number"},
        applicationName: "MongoDB Shell",
    };

    const cursor = coll.aggregate(
        [
            {$match: {v: {$gt: 0, $lt: 5}}},
            {$project: {hello: true}},
        ],
        {cursor: {batchSize: 1}});  // returns 1 doc

    // Since the cursor hasn't been exhausted yet, ensure no query stats results have been written
    // yet.
    let queryStats = getQueryStats(db);
    assert.eq(0, queryStats.length, queryStats);

    // Run a getMore to exhaust the cursor, then ensure query stats results have been written
    // accurately. batchSize must be 2 so the cursor recognizes exhaustion.
    assert.commandWorked(db.runCommand({
        getMore: cursor.getId(),
        collection: coll.getName(),
        batchSize: 2
    }));  // returns 1 doc, exhausts the cursor
    queryStats = getQueryStatsAggCmd(db);
    assert.eq(1, queryStats.length, queryStats);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 2,
                          /* expectedDocsReturnedSumOfSq */ 4,
                          /* getMores */ true);

    // Run more queries (to exhaustion) with the same query shape, and ensure query stats results
    // are accurate.
    coll.aggregate([
        {$match: {v: {$gt: 0, $lt: 5}}},
        {$project: {hello: true}},
    ]);  // returns 2 docs
    coll.aggregate([
        {$match: {v: {$gt: 2, $lt: 3}}},
        {$project: {hello: true}},
    ]);  // returns 0 docs
    coll.aggregate([
        {$match: {v: {$gt: 0, $lt: 2}}},
        {$project: {hello: true}},
    ]);  // returns 1 doc
    queryStats = getQueryStatsAggCmd(db);
    assert.eq(1, queryStats.length, queryStats);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 4,
                          /* expectedDocsReturnedSum */ 5,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 0,
                          /* expectedDocsReturnedSumOfSq */ 9,
                          /* getMores */ true);
}

// Assert on batchSize-limited agg queries that killCursors will write metrics with partial results
// to the query stats store.
{
    const coll = db.coll2;
    coll.insert({v: 1});
    coll.insert({v: 4});

    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll2"},
            command: "aggregate",
            pipeline: [{$match: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}}]
        },
        cursor: {batchSize: "?number"},
        applicationName: "MongoDB Shell",
    };

    const cursor1 = coll.aggregate(
        [
            {$match: {v: {$gt: 0, $lt: 5}}},
        ],
        {cursor: {batchSize: 1}});  // returns 1 doc
    const cursor2 = coll.aggregate(
        [
            {$match: {v: {$gt: 0, $lt: 2}}},
        ],
        {cursor: {batchSize: 1}});  // returns 1 doc

    assert.commandWorked(
        db.runCommand({killCursors: coll.getName(), cursors: [cursor1.getId(), cursor2.getId()]}));
    const queryStats = getLatestQueryStatsEntry(db, {collName: coll.getName()});
    assertExpectedResults(queryStats,
                          queryStatsKey,
                          /* expectedExecCount */ 2,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 2,
                          /* getMores */ false);
}

// SERVER-83964 Test that query stats are collected if the database doesn't exist.
{
    const nonExistentDB = db.getSiblingDB("newDB");
    assert.eq([], nonExistentDB.anything.aggregate([]).toArray());
    const entry = getLatestQueryStatsEntry(db, {collName: "anything"});
    assert.neq(null, entry);
}
st.stop();
