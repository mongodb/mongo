/**
 * Ensures that query hashes are stable across timeseries. Also ensures the collection type is correct as are the metrics.
 */
import {
    getQueryStats,
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getQueryPlannerMetrics,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn, sharded = false) {
    const testDB = sharded ? conn.s.getDB("test") : conn.getDB("test");
    const collectionType = "timeseries";
    const collectionName = jsTestName() + "_" + collectionType;
    const timeField = "time";
    const timeseriesOptions = {timeField: timeField};
    const coll = testDB[collectionName];
    if (sharded) {
        conn.adminCommand({
            shardCollection: coll.getFullName(),
            key: {[timeField]: 1},
            timeseries: timeseriesOptions,
        });
    } else {
        assert.commandWorked(testDB.createCollection(collectionName, {timeseries: timeseriesOptions}));
    }
    const parameterDB = sharded ? conn.shard0 : testDB;
    const spillParameter = parameterDB.adminCommand({
        getParameter: 1,
        internalQueryEnableAggressiveSpillsInGroup: 1,
    });
    const aggressiveSpillsInGroup = spillParameter["internalQueryEnableAggressiveSpillsInGroup"];
    coll.insert({v: 1, time: ISODate("2021-05-18T00:00:00.000Z")});
    coll.insert({v: 2, time: ISODate("2021-05-18T01:00:00.000Z")});
    coll.insert({v: 3, time: ISODate("2021-05-18T02:00:00.000Z")});

    coll.find({v: 1}).toArray();
    coll.aggregate([{$match: {v: 1}}]).toArray();
    coll.count({v: 1});
    coll.distinct("v");

    let queryStats = [];
    // TODO SERVER-76263 remove special casing once sharding supports collectionType.
    if (!sharded) {
        // This ensures that the collectionType was correct.
        queryStats = getQueryStats(testDB, {
            extraMatch: {"key.collectionType": collectionType, "key.queryShape.cmdNs.coll": collectionName},
        });
    } else {
        queryStats = getQueryStats(testDB, {
            extraMatch: {"key.queryShape.cmdNs.coll": collectionName},
        });
    }
    assert.eq(
        4,
        queryStats.length,
        "Expected all four commands (find, aggregate, count, distinct) to have query stats: " + queryStats,
    );

    // Aggregate - Ensures the hash was correct.
    // This hash corresponds to the aggregation before the addition of $_internalUnpackBucket.
    let queryStatsPerQuery = getQueryStats(testDB, {
        extraMatch: {
            "key.queryShape.command": "aggregate",
            queryShapeHash: "6EAA1CF28496A4023CA7C4A624109C2E9065D9B8C2E74193FC16F57C96C56875",
        },
    });
    assert.eq(1, queryStatsPerQuery.length, "Expected the aggregate hash to match expected" + tojson(queryStats));
    assert.eq(
        1,
        queryStatsPerQuery[0].key.queryShape.pipeline.length,
        "Expected the pipeline to not include $_internalUnpackBucket.",
    );
    // Ensure correct metrics.
    assertAggregatedMetricsSingleExec(queryStatsPerQuery[0], {
        keysExamined: 0,
        docsExamined: 3,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
    });
    assertExpectedResults({
        results: queryStatsPerQuery[0],
        expectedQueryStatsKey: queryStatsPerQuery[0].key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 1,
        expectedDocsReturnedMax: 1,
        expectedDocsReturnedMin: 1,
        expectedDocsReturnedSumOfSq: 1,
        getMores: false,
    });

    // Count - Ensures the hash was correct.
    queryStatsPerQuery = getQueryStats(testDB, {
        extraMatch: {
            "key.queryShape.command": "count",
            queryShapeHash: "9588ADDD5A1D8B65C48617883FDB0C764AE3F36965DA6615D9409424E9E24935",
        },
    });
    assert.eq(1, queryStatsPerQuery.length, "Expected the count hash to match expected" + tojson(queryStats));
    // Ensure correct metrics.
    assertAggregatedMetricsSingleExec(queryStatsPerQuery[0], {
        keysExamined: 0,
        docsExamined: 3,
        hasSortStage: false,
        // Don't validate the value of 'usedDisk' here. On some variants this can actually be true,
        // but this test is not concerned with validating that.
        usedDisk: getQueryPlannerMetrics(queryStatsPerQuery[0].metrics).usedDisk["true"] > 0,
        fromMultiPlanner: false,
        fromPlanCache: false,
    });
    assertExpectedResults({
        results: queryStatsPerQuery[0],
        expectedQueryStatsKey: queryStatsPerQuery[0].key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 1,
        expectedDocsReturnedMax: 1,
        expectedDocsReturnedMin: 1,
        expectedDocsReturnedSumOfSq: 1,
        getMores: false,
    });

    // Distinct - Ensures the hash was correct.
    queryStatsPerQuery = getQueryStats(testDB, {
        extraMatch: {
            "key.queryShape.command": "distinct",
            queryShapeHash: "28E320CE6D5B70EED846BD71772ABC5F3FC363B2B21D0371DE353D353A6EFD22",
        },
    });
    assert.eq(1, queryStatsPerQuery.length, "Expected the distinct hash to match expected" + tojson(queryStats));
    // Ensure correct metrics.
    assertAggregatedMetricsSingleExec(queryStatsPerQuery[0], {
        keysExamined: 0,
        docsExamined: 3,
        hasSortStage: false,
        usedDisk: aggressiveSpillsInGroup,
        fromMultiPlanner: false,
        fromPlanCache: false,
    });
    assertExpectedResults({
        results: queryStatsPerQuery[0],
        expectedQueryStatsKey: queryStatsPerQuery[0].key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 3,
        expectedDocsReturnedMax: 3,
        expectedDocsReturnedMin: 3,
        expectedDocsReturnedSumOfSq: 9,
        getMores: false,
    });

    // Find - Ensures the hash was correct.
    queryStatsPerQuery = getQueryStats(testDB, {
        extraMatch: {
            "key.queryShape.command": "find",
            queryShapeHash: "B168ABE3917F52922BC07512C28E81E40C963EB822423731D63786927942061F",
        },
    });
    assert.eq(1, queryStatsPerQuery.length, "Expected the find hash to match expected" + tojson(queryStats));
    // Ensure correct metrics.
    assertAggregatedMetricsSingleExec(queryStatsPerQuery[0], {
        keysExamined: 0,
        docsExamined: 3,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
    });
    assertExpectedResults({
        results: queryStatsPerQuery[0],
        expectedQueryStatsKey: queryStatsPerQuery[0].key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 1,
        expectedDocsReturnedMax: 1,
        expectedDocsReturnedMin: 1,
        expectedDocsReturnedSumOfSq: 1,
        getMores: false,
    });
}

const options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

// Test Unsharded
{
    const conn = MongoRunner.runMongod(options);
    runTest(conn);
    MongoRunner.stopMongod(conn);
}

// Test Sharded
{
    const st = new ShardingTest({shards: 2, mongosOptions: options});
    runTest(st, true);
    st.stop();
}
