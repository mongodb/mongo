/**
 * Test that query stats are collected properly when featureFlagQueryStatsFindCommand is disabled
 * but featureFlagQueryStats is enabled. Metrics should be collected for find and agg queries.
 * TODO SERVER-79494 remove this test once featureFlagQueryStatsFindCommand is removed.
 */
load("jstests/libs/query_stats_utils.js");
load("jstests/libs/feature_flag_util.js");  // For 'FeatureFlagUtil'

(function() {
"use strict";

function runTest(conn) {
    const testDB = conn.getDB('test');
    let coll = testDB[jsTestName()];
    coll.drop();

    coll.insert({x: 0});
    coll.insert({x: 1});
    coll.insert({x: 2});

    let res = coll.aggregate([{$match: {x: 2}}]).toArray();
    assert.eq(1, res.length, res);

    let queryStats = getQueryStats(conn);
    // Ensure metrics were collected for the agg query.
    assert.eq(1, queryStats.length, queryStats);

    res = coll.find({x: 2}).toArray();
    assert.eq(1, res.length, res);
    queryStats = getQueryStats(conn);
    // Ensure we collected metrics for the find query, agg query, and additional agg query from
    // calling $queryStats.
    assert.eq(3, queryStats.length, queryStats);
    verifyMetrics(queryStats);
}

const conn = MongoRunner.runMongod({
    setParameter: {internalQueryStatsRateLimit: -1, featureFlagQueryStats: true},
});
const testDB = conn.getDB('test');
if (FeatureFlagUtil.isEnabled(testDB, "QueryStatsFindCommand")) {
    jsTestLog("Skipping test since featureFlagQueryStatsFindCommand is on.");
    MongoRunner.stopMongod(conn);
    quit();
}
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryStatsRateLimit: -1,
            featureFlagQueryStats: true,
        }
    }
});
runTest(st.s);
st.stop();
}());