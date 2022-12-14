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

load('jstests/libs/cluster_server_parameter_utils.js');

function runTest(st, startupRefreshIntervalMS) {
    // This assert is necessary because we subtract 8000 MS from this value later on, and we don't
    // want the interval to go below 1 second.
    assert(startupRefreshIntervalMS >= 9000);
    // First, check that the mongos logs a refresh attempt within the first refreshIntervalMS
    // milliseconds that finds no documents on the config servers.
    const conn = st.s0;
    const errorMarginMS = 5000;
    const startupRefreshIntervalRelaxedMS = startupRefreshIntervalMS + errorMarginMS;
    let expectedParams =
        Object.fromEntries(kAllClusterParameterDefaults.map(elem => [elem._id, elem]));
    function expectedParamsAsArray() {
        return Object.values(expectedParams);
    }

    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {clusterParameterDocuments: [{"tenantId": "none", "updatedParameters": []}]},
        1,
        startupRefreshIntervalRelaxedMS);
    runGetClusterParameterSharded(st, '*', expectedParamsAsArray());

    // Set a cluster parameter to a different value and then wait.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2022}}}));
    expectedParams.testIntClusterParameter.intData = 2022;

    // Check that the newly set parameter is refreshed within the interval.
    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {
            clusterParameterDocuments: [{
                "tenantId": "none",
                "updatedParameters": [{_id: "testIntClusterParameter", intData: 2022}]
            }]
        },
        1,
        startupRefreshIntervalRelaxedMS);
    runGetClusterParameterSharded(st, '*', expectedParamsAsArray());

    // Set another cluster parameter and check that only the updated parameter is refreshed within
    // the interval.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testStrClusterParameter: {strData: "welcome"}}}));
    expectedParams.testStrClusterParameter.strData = "welcome";

    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {
            clusterParameterDocuments: [{
                "tenantId": "none",
                "updatedParameters": [{_id: "testStrClusterParameter", strData: "welcome"}]
            }]
        },
        1,
        startupRefreshIntervalRelaxedMS);
    runGetClusterParameterSharded(st, '*', expectedParamsAsArray());

    // Shorten the refresh interval by error margin + 3 seconds and verify that further
    // setClusterParameter updates are seen within the new interval.
    const newRefreshIntervalMS = startupRefreshIntervalMS - errorMarginMS - 3000;
    const newRefreshIntervalRelaxedMS = newRefreshIntervalMS + errorMarginMS;
    assert.commandWorked(conn.adminCommand(
        {setParameter: 1, clusterServerParameterRefreshIntervalSecs: newRefreshIntervalMS / 1000}));
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2025}}}));
    expectedParams.testIntClusterParameter.intData = 2025;

    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {
            clusterParameterDocuments: [{
                "tenantId": "none",
                "updatedParameters": [{_id: "testIntClusterParameter", intData: 2025}]
            }]
        },
        1,
        newRefreshIntervalRelaxedMS);
    runGetClusterParameterSharded(st, '*', expectedParamsAsArray());

    // Restart the mongos and check that it refreshes both of the parameters that have documents on
    // the config server.
    st.restartMongos(0);
    checkLog.containsRelaxedJson(conn,
                                 6226403,
                                 {
                                     clusterParameterDocuments: [{
                                         "tenantId": "none",
                                         updatedParameters: [
                                             {_id: "testIntClusterParameter", intData: 2025},
                                             {_id: "testStrClusterParameter", strData: "welcome"}
                                         ]
                                     }]
                                 },
                                 1,
                                 newRefreshIntervalRelaxedMS);
    runGetClusterParameterSharded(st, '*', expectedParamsAsArray());

    // Check that single parameter updates are caught as expected after restart. Note that the
    // startup refresh interval is used since runtime setParameter updates are not persisted.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testStrClusterParameter: {strData: "goodbye"}}}));
    expectedParams.testStrClusterParameter.strData = "goodbye";

    checkLog.containsRelaxedJson(
        conn,
        6226403,
        {
            clusterParameterDocuments: [{
                "tenantId": "none",
                "updatedParameters": [{_id: "testStrClusterParameter", strData: "goodbye"}]
            }]
        },
        1,
        startupRefreshIntervalRelaxedMS);
    runGetClusterParameterSharded(st, '*', expectedParamsAsArray());

    // Finally, check that multiple setClusterParameter invocations within a single refresh cycle
    // are captured and updated.
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2028}}}));
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {testBoolClusterParameter: {boolData: true}}}));
    expectedParams.testIntClusterParameter.intData = 2028;
    expectedParams.testBoolClusterParameter.boolData = true;

    checkLog.containsRelaxedJson(conn,
                                 6226403,
                                 {
                                     clusterParameterDocuments: [{
                                         "tenantId": "none",
                                         "updatedParameters": [
                                             {_id: "testIntClusterParameter", intData: 2028},
                                             {_id: "testBoolClusterParameter", boolData: true}

                                         ]
                                     }]
                                 },
                                 1,
                                 startupRefreshIntervalRelaxedMS);
    runGetClusterParameterSharded(st, '*', expectedParamsAsArray());
}

// Test with shortened clusterServerParameterRefreshIntervalSecs at startup.
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
            clusterServerParameterRefreshIntervalSecs: 10,
        },
    },
};
let st = new ShardingTest(options);
runTest(st, 10000);
st.stop();
})();
