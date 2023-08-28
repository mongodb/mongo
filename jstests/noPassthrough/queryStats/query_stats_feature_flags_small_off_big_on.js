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

// Disable via TestData so there's no conflict in case a variant has all flags enabled.
TestData.setParameters.featureFlagQueryStatsFindCommand = false;
TestData.setParameters.featureFlagQueryStats = true;
const conn = MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}});
assert.neq(null, conn, 'failed to start mongod');
runTest(conn);
MongoRunner.stopMongod(conn);

TestData.setParametersMongos.featureFlagQueryStatsFindCommand = false;
TestData.setParametersMongos.featureFlagQueryStats = true;
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
    }
});
runTest(st.s);
st.stop();
