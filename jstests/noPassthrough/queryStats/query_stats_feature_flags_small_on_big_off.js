/**
 * Test that query stats are collected properly when featureFlagQueryStatsFindCommand is enabled but
 * featureFlagQueryStats is disabled. Metrics for find commands should be collected, but metrics for
 * aggregate commmands should not be collected.
 * TODO SERVER-79494 remove this test once featureFlagQueryStatsFindCommand is removed.
 */
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
    // Ensure no metrics have been colleted so far.
    assert.eq(0, queryStats.length, queryStats);

    res = coll.find({x: 2}).toArray();
    assert.eq(1, res.length, res);
    queryStats = getQueryStats(conn);
    // Ensure we collected metrics for the find command.
    assert.eq(1, queryStats.length, queryStats);
    const entry = queryStats[0];
    assert.eq("find", entry.key.queryShape.command);
    assert.eq({"x": {$eq: "?number"}}, entry.key.queryShape.filter);
    verifyMetrics(queryStats);
}

// Disable via TestData so there's no conflict in case a variant has all flags enabled.
TestData.setParameters.featureFlagQueryStatsFindCommand = true;
TestData.setParameters.featureFlagQueryStats = false;
const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
    },
});
assert.neq(null, conn, 'failed to start mongod');
runTest(conn);
MongoRunner.stopMongod(conn);

TestData.setParametersMongos.featureFlagQueryStatsFindCommand = true;
TestData.setParametersMongos.featureFlagQueryStats = false;
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
