// Test getParameter on mongod and mongos, including the with-detail syntax.
//
// @tags: [requires_replication, requires_sharding]

(() => {
    'use strict';
    const st = new ShardingTest({shards: 1, mongos: 1});
    const mongosDB = st.s0.getDB("admin");
    const shardDB = st.shard0.getDB("admin");

    function checkSpecificParameters(dbConn, parameters) {
        const plainCommand = {getParameter: 1};
        const detailCommand = {getParameter: {showDetails: true}};

        // Append requests to retrieve specific parameters.
        parameters.forEach((parameter) => {
            plainCommand[parameter["name"]] = 1;
            detailCommand[parameter["name"]] = 1;
        });

        // Fetch results
        const resultsPlain = assert.commandWorked(dbConn.adminCommand(plainCommand));
        const resultsWithDetail = assert.commandWorked(dbConn.adminCommand(detailCommand));

        // Ensure requested parameters had expected values and detail information.
        parameters.forEach((parameter) => {
            const expectedDetailedResultObj = parameter["result"];
            const expectedValue = expectedDetailedResultObj["value"];
            const plainErrMsg = "Expecting plain result to contain: " + tojson(parameter) +
                "but found: " + tojson(resultsPlain);
            const detailErrMsg = "Expecting detail result to contain: " + tojson(parameter) +
                "but found: " + tojson(resultsWithDetail);

            assert.eq(resultsPlain[parameter["name"]], expectedValue, plainErrMsg);
            assert.docEq(
                expectedDetailedResultObj, resultsWithDetail[parameter["name"]], detailErrMsg);
        });
    }

    function checkAllParameters(dbConn) {
        const plainCommand = {getParameter: '*'};
        const detailCommand = {getParameter: {showDetails: true, allParameters: true}};

        // Fetch results
        let resultsPlain = assert.commandWorked(dbConn.adminCommand(plainCommand));
        let resultsWithDetail = assert.commandWorked(dbConn.adminCommand(detailCommand));

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
    }

    // Each of the entries in the following array is an object with two keys, "name" and "result".
    // "name" is the name of a server parameter, and "result" is the expected object
    // getParameter should return for that parameter, when details are requested.
    const specificParametersBothProcesses = [
        {
            name: "ShardingTaskExecutorPoolMinSize",
            result: {value: 1, settableAtRuntime: true, settableAtStartup: true}
        },
        {
            name: "maxLogSizeKB",
            result: {value: 10, settableAtRuntime: true, settableAtStartup: true}
        },
        {
            name: "cursorTimeoutMillis",
            result: {value: NumberLong(600000), settableAtRuntime: true, settableAtStartup: true}
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
    ];

    checkSpecificParameters(mongosDB, specificParametersBothProcesses);
    checkSpecificParameters(shardDB, specificParametersBothProcesses);

    checkSpecificParameters(shardDB, specificParametersMongodOnly);
    checkSpecificParameters(mongosDB, specificParametersMongosOnly);

    checkAllParameters(mongosDB);
    checkAllParameters(shardDB);

    st.stop();
})();
