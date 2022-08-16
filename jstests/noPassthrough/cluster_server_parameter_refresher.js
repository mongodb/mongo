/**
 * Checks that the mongos cluster server parameter refresh job runs as expected.
 *
 * @tags: [
 *   # Requires all nodes to be running at least 6.1.
 *   requires_fcv_61,
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_sharding
 *  ]
 */
(function() {
'use strict';

function runTest(st, startupRefreshIntervalMS) {
    // First, check that the mongos logs a refresh attempt within the first refreshIntervalMS
    // milliseconds that finds no documents on the config servers.
    const conn = st.s0;
    const errorMarginMS = 5000;
    const startupRefreshIntervalRelaxedMS = startupRefreshIntervalMS + errorMarginMS;
    checkLog.containsRelaxedJson(
        conn, 6226403, {clusterParameterDocuments: []}, 1, startupRefreshIntervalRelaxedMS);

    // Set a cluster parameter to a different value and then wait.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2022}}}));

    // Check that the newly set parameter is refreshed within the interval.
    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {clusterParameterDocuments: [{_id: "testIntClusterParameter", intData: 2022}]},
        1,
        startupRefreshIntervalRelaxedMS);

    // Set another cluster parameter and check that only the updated parameter is refreshed within
    // the interval.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testStrClusterParameter: {strData: "welcome"}}}));
    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {clusterParameterDocuments: [{_id: "testStrClusterParameter", strData: "welcome"}]},
        1,
        startupRefreshIntervalRelaxedMS);

    // Shorten the refresh interval by half and verify that the next setClusterParameter update
    // is seen within the new interval.
    const newRefreshIntervalMS = startupRefreshIntervalMS / 2;
    const newRefreshIntervalSecs = newRefreshIntervalMS / 1000;
    const newRefreshIntervalRelaxedMS = newRefreshIntervalMS + errorMarginMS;
    assert.commandWorked(conn.adminCommand(
        {setParameter: 1, clusterServerParameterRefreshIntervalSecs: newRefreshIntervalSecs}));
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2025}}}));
    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {clusterParameterDocuments: [{_id: "testIntClusterParameter", intData: 2025}]},
        1,
        newRefreshIntervalRelaxedMS);

    // Restart the mongos and check that it refreshes both of the parameters that have documents on
    // the config server.
    st.restartMongos(0);
    checkLog.containsRelaxedJson(conn,
                                 6226403,
                                 {
                                     clusterParameterDocuments: [
                                         {_id: "testIntClusterParameter", intData: 2025},
                                         {_id: "testStrClusterParameter", strData: "welcome"}
                                     ]
                                 },
                                 1,
                                 newRefreshIntervalRelaxedMS);

    // Check that single parameter updates are caught as expected after restart. Note that the
    // startup refresh interval is used since runtime setParameter updates are not persisted.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testStrClusterParameter: {strData: "goodbye"}}}));
    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {clusterParameterDocuments: [{_id: "testStrClusterParameter", strData: "goodbye"}]},
        1,
        startupRefreshIntervalRelaxedMS);

    // Finally, check that multiple setClusterParameter invocations within a single refresh cycle
    // are captured and updated.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2028}}}));
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testBoolClusterParameter: {boolData: true}}}));
    checkLog.containsRelaxedJson(conn,
                                 6226403,
                                 {
                                     clusterParameterDocuments: [
                                         {_id: "testIntClusterParameter", intData: 2028},
                                         {_id: "testBoolClusterParameter", boolData: true}
                                     ]
                                 },
                                 1,
                                 startupRefreshIntervalRelaxedMS);
}

// Test with default clusterServerParameterRefreshIntervalSecs at startup.
let options = {
    mongos: 1,
    config: 1,
    shards: 3,
    rs: {
        nodes: 3,
    },
    mongosOptions: {
        setParameter: {
            logLevel: 3,
        },
    },
};
let st = new ShardingTest(options);
runTest(st, 30000);
st.stop();

// Test with non-default clusterServerParameterRefreshIntervalSecs at startup.
options = {
    mongos: 1,
    config: 1,
    shards: 3,
    rs: {
        nodes: 3,
    },
    mongosOptions: {
        setParameter: {
            logLevel: 3,
            clusterServerParameterRefreshIntervalSecs: 20,
        },
    },
};
st = new ShardingTest(options);
runTest(st, 20000);
st.stop();
})();
