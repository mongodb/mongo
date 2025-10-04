/**
 * Checks that the mongos cluster server parameter refresh job runs as expected.
 *
 * @tags: [
 *   # Requires all nodes to be running at least 6.3.
 *   requires_fcv_63,
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_sharding
 *  ]
 */
import {
    kAllClusterParameterDefaults,
    kAllClusterParameterSetInternallyNames,
    runGetClusterParameterSharded,
} from "jstests/libs/cluster_server_parameter_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(st, startupRefreshIntervalMS) {
    // This assert is necessary because we subtract 8000 MS from this value later on, and we don't
    // want the interval to go below 1 second.
    assert(startupRefreshIntervalMS >= 9000);
    // First, check that the mongos logs a refresh attempt within the first refreshIntervalMS
    // milliseconds that finds no documents on the config servers.
    const conn = st.s0;
    const errorMarginMS = 10000;
    const kRefreshLogId = 6226403;
    const startupRefreshIntervalRelaxedMS = startupRefreshIntervalMS * 2 + errorMarginMS;
    let expectedParams = {};
    const defaultParams = Object.fromEntries(kAllClusterParameterDefaults.map((elem) => [elem._id, elem]));

    function assertParams(timeoutMillis) {
        let expectedLogged = Object.keys(expectedParams).map((key) => {
            return {_id: key, ...expectedParams[key]};
        });
        expectedLogged.sort(bsonWoCompare);
        assert.soon(
            function () {
                const logMessages = checkLog.getGlobalLog(conn);
                if (logMessages === null) {
                    return false;
                }

                for (let logMsg of logMessages) {
                    let obj;
                    try {
                        obj = JSON.parse(logMsg);
                    } catch (ex) {
                        print("JSON.parse() failed: " + tojson(ex) + ": " + logMsg);
                        throw ex;
                    }

                    if (obj.id === kRefreshLogId) {
                        let cpd = obj.attr.clusterParameterDocuments[0].updatedParameters;
                        // We may observe updates to parameters done by MongoDB itself; exclude them
                        cpd = cpd.filter((elem) => !kAllClusterParameterSetInternallyNames.includes(elem._id));
                        cpd.sort(bsonWoCompare);
                        if (bsonWoCompare(cpd, expectedLogged) === 0) {
                            return true;
                        }
                    }
                }
                return false;
            },
            "Could not find matching log entry",
            timeoutMillis,
            300,
            {runHangAnalyzer: false},
        );

        runGetClusterParameterSharded(st, "*", Object.values({...defaultParams, ...expectedParams}));
    }

    assertParams(startupRefreshIntervalRelaxedMS);

    // Set a cluster parameter to a different value and then wait.
    assert.commandWorked(conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2022}}}));
    expectedParams.testIntClusterParameter = {_id: "testIntClusterParameter", intData: 2022};

    // Check that the newly set parameter is refreshed within the interval.
    assertParams(startupRefreshIntervalRelaxedMS);

    // Set another cluster parameter and check that both parameters are refreshed.
    assert.commandWorked(conn.adminCommand({setClusterParameter: {testStrClusterParameter: {strData: "welcome"}}}));
    expectedParams.testStrClusterParameter = {_id: "testStrClusterParameter", strData: "welcome"};

    assertParams(startupRefreshIntervalRelaxedMS);

    // Ensure that updates to the refresh interval take effect correctly.
    const newRefreshIntervalMS = startupRefreshIntervalMS - 8000;
    const newRefreshIntervalRelaxedMS = newRefreshIntervalMS * 2 + errorMarginMS;
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, clusterServerParameterRefreshIntervalSecs: newRefreshIntervalMS / 1000}),
    );
    assert.commandWorked(conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2025}}}));
    expectedParams.testIntClusterParameter.intData = 2025;

    assertParams(newRefreshIntervalRelaxedMS);

    // Restart the mongos and check that it refreshes both of the parameters that have documents on
    // the config server. Note that the startup refresh interval is used since runtime setParameter
    // updates are not persisted.
    st.restartMongos(0);
    assertParams(startupRefreshIntervalRelaxedMS);

    // Check that single parameter updates are caught as expected after restart.
    assert.commandWorked(conn.adminCommand({setClusterParameter: {testStrClusterParameter: {strData: "goodbye"}}}));
    expectedParams.testStrClusterParameter.strData = "goodbye";

    assertParams(startupRefreshIntervalRelaxedMS);

    // Check that multiple setClusterParameter invocations within a single refresh cycle
    // are captured and updated.
    assert.commandWorked(conn.adminCommand({setClusterParameter: {testIntClusterParameter: {intData: 2028}}}));
    assert.commandWorked(conn.adminCommand({setClusterParameter: {testBoolClusterParameter: {boolData: true}}}));
    expectedParams.testIntClusterParameter.intData = 2028;
    expectedParams.testBoolClusterParameter = {_id: "testBoolClusterParameter", boolData: true};

    assertParams(startupRefreshIntervalRelaxedMS);

    // Ensure that deletes are captured and properly refreshed.
    [st.configRS, ...st._rs.map((rs) => rs.test)].forEach((rs) => {
        const db = rs.getPrimary().getDB("config");
        assert.commandWorked(db.clusterParameters.remove({_id: "testIntClusterParameter"}));
    });
    delete expectedParams.testIntClusterParameter;

    assertParams(startupRefreshIntervalRelaxedMS);

    // Try deleting the whole collection and make sure that it's properly refreshed.
    [st.configRS, ...st._rs.map((rs) => rs.test)].forEach((rs) => {
        assert(rs.getPrimary().getDB("config").clusterParameters.drop());
    });

    // Perform a dummy write in order to get the config shard's cluster time cached on the mongos.
    st.s.getDB("config").abc.insert({a: "hello"});

    expectedParams = {};
    assertParams(startupRefreshIntervalRelaxedMS);
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
