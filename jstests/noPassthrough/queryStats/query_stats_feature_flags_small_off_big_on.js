/**
 * Test that query stats are collected properly when featureFlagQueryStatsFindCommand is disabled
 * but featureFlagQueryStats is enabled. Metrics should be collected for find and agg queries.
 * TODO SERVER-79494 remove this test once featureFlagQueryStatsFindCommand is removed.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getQueryStats, verifyMetrics} from "jstests/libs/query_stats_utils.js";

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
    setParameter: {
        internalQueryStatsRateLimit: -1,
        featureFlagQueryStatsFindCommand: false,
        featureFlagQueryStats: true
    }
});
assert.neq(null, conn, 'failed to start mongod');
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    mongosOptions: {
        setParameter: {
            internalQueryStatsRateLimit: -1,
            'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}",
            featureFlagQueryStatsFindCommand: false,
            featureFlagQueryStats: true
        }
    },
    config: 1,
    configOptions:
        {setParameter: {featureFlagQueryStatsFindCommand: false, featureFlagQueryStats: true}},
    shards: 1,
    rs: {nodes: 1},
    rsOptions:
        {setParameter: {featureFlagQueryStatsFindCommand: false, featureFlagQueryStats: true}}
});
runTest(st.s);
st.stop();
