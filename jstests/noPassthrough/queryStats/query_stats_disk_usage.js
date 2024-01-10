/**
 * Test that mongos is collecting query stats metrics for find queries.
 * @tags: [
 * featureFlagQueryStatsDataBearingNodes,
 * ]
 */

import {
    assertAggregatedBoolean,
    assertAggregatedMetric,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats
} from "jstests/libs/query_stats_utils.js";

function runTest(conn) {
    const db = conn.getDB("test");

    const collName = "coll";
    const coll = db[collName];

    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            command: "find",
            filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
            sort: {y: 1}
        },
        collectionType: "collection",
        batchSize: "?number",
        client: {application: {name: "MongoDB Shell"}}
    };

    coll.insert({v: 1, y: 1});

    const cursor = coll.find({v: {$gt: 0, $lt: 5}}).sort({y: 1}).batchSize(1);

    // Since the cursor hasn't been exhausted yet, ensure no query stats results have been written
    // yet.
    let queryStats = getQueryStats(db);
    assert.eq(0, queryStats.length, queryStats);

    // Run a getMore to exhaust the cursor, then ensure query stats results have been written
    // accurately. batchSize must be 2 so the cursor recognizes exhaustion.
    assert.commandWorked(
        db.runCommand({getMore: cursor.getId(), collection: coll.getName(), batchSize: 2}));

    queryStats = getLatestQueryStatsEntry(conn);
    print("Query Stats: " + tojson(queryStats));

    assertExpectedResults(queryStats,
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 1,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 1,
                          /* getMores */ true);

    assertAggregatedMetric(queryStats, "keysExamined", {sum: 0, min: 0, max: 0, sumOfSq: 0});
    assertAggregatedMetric(queryStats, "docsExamined", {sum: 1, min: 1, max: 1, sumOfSq: 1});

    // true, false
    assertAggregatedBoolean(queryStats, "hasSortStage", {trueCount: 1, falseCount: 0});
    assertAggregatedBoolean(queryStats, "usedDisk", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromMultiPlanner", {trueCount: 0, falseCount: 1});
    assertAggregatedBoolean(queryStats, "fromPlanCache", {trueCount: 0, falseCount: 1});
}

const options = {
    setParameter: {internalQueryStatsRateLimit: -1, featureFlagQueryStatsDataBearingNodes: 1}
};

jsTestLog("Standalone: Testing query stats disk usage for mongod");
{
    const conn = MongoRunner.runMongod(options);
    runTest(conn);
    MongoRunner.stopMongod(conn);
}

// SERVER-83688 - Add mongos variant of this test
