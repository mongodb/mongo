// Test getParameter on mongod and mongos, including the with-detail syntax.
//
// @tags: [requires_replication, requires_sharding]

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, mongos: 1});
const mongosDB = st.s0.getDB("admin");
const shardDB = st.shard0.getDB("admin");

function checkSpecificParameters(dbConn, parameters) {
    const plainCommand = {getParameter: 1};
    const detailCommand = {getParameter: {showDetails: true}};
    const setAtStartupCommand = {getParameter: {setAt: "startup"}};
    const setAtRuntimeCommand = {getParameter: {setAt: "runtime"}};

    // Append requests to retrieve specific parameters.
    parameters.forEach((parameter) => {
        plainCommand[parameter["name"]] = 1;
        detailCommand[parameter["name"]] = 1;
        setAtStartupCommand[parameter["name"]] = 1;
        setAtRuntimeCommand[parameter["name"]] = 1;
    });

    // Fetch results
    const resultsPlain = assert.commandWorked(dbConn.adminCommand(plainCommand));
    const resultsWithDetail = assert.commandWorked(dbConn.adminCommand(detailCommand));
    const startupResults = assert.commandWorked(dbConn.adminCommand(setAtStartupCommand));
    const runtimeResults = assert.commandWorked(dbConn.adminCommand(setAtRuntimeCommand));

    // Ensure requested parameters had expected values and detail information.
    parameters.forEach((parameter) => {
        const expectedDetailedResultObj = parameter["result"];
        const expectedValue = expectedDetailedResultObj["value"];
        const plainErrMsg = "Expecting plain result to contain: " + tojson(parameter) +
            "but found: " + tojson(resultsPlain);
        const detailErrMsg = "Expecting detail result to contain: " + tojson(parameter) +
            "but found: " + tojson(resultsWithDetail);
        const startupErrMsg = "Expecting startup-only result to contain: " + tojson(parameter) +
            "but found: " + tojson(startupResults);
        const runtimeErrMsg = "Expecting runtime-only result to contain: " + tojson(parameter) +
            "but found: " + tojson(runtimeResults);
        const unexpectedStartupErrMsg =
            "Expecting startup-only result to omit: " + tojson(parameter) +
            "but found: " + tojson(startupResults);
        const unexpectedRuntimeErrMsg =
            "Expecting runtime-only result to omit: " + tojson(parameter) +
            "but found: " + tojson(runtimeResults);

        assert.eq(resultsPlain[parameter["name"]], expectedValue, plainErrMsg);
        assert.docEq(expectedDetailedResultObj, resultsWithDetail[parameter["name"]], detailErrMsg);

        // If the parameter is settable at startup, assert that it appears in startupResults
        if (expectedDetailedResultObj["settableAtStartup"] === true) {
            assert.eq(startupResults[parameter["name"]], expectedValue, startupErrMsg);
        }

        // If the parameter is not settable at startup, assert that it is omitted from
        // startupResults.
        if (expectedDetailedResultObj["settableAtStartup"] === false) {
            assert(!startupResults[parameter["name"]], unexpectedStartupErrMsg);
        }

        // If the parameter is settable at runtime, assert that it appears in runtimeResults.
        if (expectedDetailedResultObj["settableAtRuntime"] === true) {
            assert.eq(runtimeResults[parameter["name"]], expectedValue, runtimeErrMsg);
        }

        // If the parameter is not settable at runtime, assert that it is omitted from
        // runtimeResults.
        if (expectedDetailedResultObj["settableAtRuntime"] === false) {
            assert(!runtimeResults[parameter["name"]], unexpectedRuntimeErrMsg);
        }
    });
}

function checkAllParameters(dbConn) {
    const plainCommand = {getParameter: '*'};
    const detailCommand = {getParameter: {showDetails: true, allParameters: true}};
    const setAtStartupCommand = {
        getParameter: {setAt: "startup", allParameters: true, showDetails: true}
    };
    const setAtRuntimeCommand = {
        getParameter: {setAt: "runtime", allParameters: true, showDetails: true}
    };

    // Fetch results
    let resultsPlain = assert.commandWorked(dbConn.adminCommand(plainCommand));
    let resultsWithDetail = assert.commandWorked(dbConn.adminCommand(detailCommand));
    const startupResults = assert.commandWorked(dbConn.adminCommand(setAtStartupCommand));
    const runtimeResults = assert.commandWorked(dbConn.adminCommand(setAtRuntimeCommand));

    // Ensure results are consistent between syntaxes. We don't check for explicit expected
    // values here to avoid the need to specify them for every parameter and update this file
    // every time one is added or changes in value.
    Object.keys(resultsWithDetail).forEach((k) => {
        if (resultsWithDetail[k].hasOwnProperty("value")) {
            assert.eq(resultsWithDetail[k]["value"],
                      resultsPlain[k],
                      "In all parameters, mismatch for parameter " + k + ":" +
                          tojson(resultsWithDetail[k]) + " vs " + tojson(resultsPlain[k]));
        }
    });
    Object.keys(resultsPlain).forEach((k) => {
        assert(resultsWithDetail.hasOwnProperty(k));
        if (resultsWithDetail[k].hasOwnProperty("value")) {
            assert.eq(resultsPlain[k], resultsWithDetail[k]["value"]);
        }
    });

    // Ensure that every parameter returned in startupResults has "settableAtStartup" set to true.
    Object.keys(startupResults).forEach((parameterName) => {
        // Ignore fields like "ok", "$clusterTime", etc. in the reply.
        if (startupResults[parameterName].hasOwnProperty("settableAtStartup")) {
            assert.eq(startupResults[parameterName]["settableAtStartup"],
                      true,
                      "In all startup parameters, unexpectedly received parameter " +
                          parameterName + " that has 'settableAtStartup' set to false: " +
                          tojson(startupResults[parameterName]));
        }
    });

    // Ensure that every parameter returned in runtimeResults has "settableAtRuntime" set to true.
    Object.keys(runtimeResults).forEach((parameterName) => {
        // Ignore fields like "ok", "$clusterTime", etc. in the reply.
        if (runtimeResults[parameterName].hasOwnProperty("settableAtRuntime")) {
            assert.eq(runtimeResults[parameterName]["settableAtRuntime"],
                      true,
                      "In all runtime parameters, unexpectedly received parameter " +
                          parameterName + " that has 'settableAtRuntime' set to false: " +
                          tojson(runtimeResults[parameterName]));
        }
    });
}

// Each of the entries in the following array is an object with two keys, "name" and "result".
// "name" is the name of a server parameter, and "result" is the expected object
// getParameter should return for that parameter, when details are requested.
const specificParametersBothProcesses = [
    {
        name: "ShardingTaskExecutorPoolMinSize",
        result: {value: 1, settableAtRuntime: true, settableAtStartup: true}
    },
    {name: "maxLogSizeKB", result: {value: 10, settableAtRuntime: true, settableAtStartup: true}},
    {
        name: "cursorTimeoutMillis",
        result: {value: NumberLong(600000), settableAtRuntime: true, settableAtStartup: true}
    },
    {
        name: "loadRoutingTableOnStartup",
        result: {value: true, settableAtRuntime: false, settableAtStartup: true}
    },
    {
        name: "clusterAuthMode",
        result: {value: "undefined", settableAtRuntime: true, settableAtStartup: false}
    }
];
const specificParametersMongodOnly = [
    {
        name: "ttlMonitorEnabled",
        result: {value: true, settableAtRuntime: true, settableAtStartup: true}
    },
    {
        name: "skipShardingConfigurationChecks",
        result: {value: false, settableAtRuntime: false, settableAtStartup: true}
    },
    {
        name: "shardedIndexConsistencyCheckIntervalMS",
        result: {value: 600000, settableAtRuntime: false, settableAtStartup: true}
    },
    {
        name: "clusterIpSourceAllowlist",
        result: {value: null, settableAtRuntime: true, settableAtStartup: false}
    }
];
const specificParametersMongosOnly = [
    {
        name: "activeFaultDurationSecs",
        result: {value: 120, settableAtRuntime: true, settableAtStartup: true}
    },
    {
        name: "userCacheInvalidationIntervalSecs",
        result: {value: 30, settableAtRuntime: true, settableAtStartup: true}
    },
    {
        name: "loadBalancerPort",
        result: {value: 0, settableAtRuntime: false, settableAtStartup: true}
    },
    {
        name: "testMongosOnlyRuntimeParameter",
        result: {value: false, settableAtRuntime: true, settableAtStartup: false}
    }
];

checkSpecificParameters(mongosDB, specificParametersBothProcesses);
checkSpecificParameters(shardDB, specificParametersBothProcesses);

checkSpecificParameters(shardDB, specificParametersMongodOnly);
checkSpecificParameters(mongosDB, specificParametersMongosOnly);

checkAllParameters(mongosDB);
checkAllParameters(shardDB);

st.stop();
