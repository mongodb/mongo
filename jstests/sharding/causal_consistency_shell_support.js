/**
 * Tests which commands support causal consistency in the Mongo shell, that for each supported
 * command, the shell properly forwards its operation and cluster time and updates them based on the
 * response, and that the server rejects commands with afterClusterTime ahead of cluster time.
 */
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

    // Verifies causal consistency is either enabled or disabled for each given command name.
    function checkCausalConsistencySupportForCommandNames(cmdObjs, isReadCommand) {
        cmdObjs.forEach(function(cmdObj) {
            let cmdName = Object.keys(cmdObj)[0];
            if (cmdName === "query" || cmdName === "$query") {
                cmdObj = cmdObj[cmdName];
                cmdName = Object.keys(cmdObj)[0];
            }

            assert.eq(testDB.getMongo()._isReadCommand(cmdObj),
                      isReadCommand,
                      "expected causal consistency support for command, " + tojson(cmdObj) +
                          ", to be " + isReadCommand);
        });
    }

    // Verifies the command works and properly updates operation or cluster time.
    function runCommandAndCheckLogicalTimes(cmdObj, db, shouldAdvance) {
        const mongo = db.getMongo();

        // Extract initial operation and cluster time.
        let operationTime = mongo.getOperationTime();
        let clusterTimeObj = mongo.getClusterTime();

        assert.commandWorked(db.runCommand(cmdObj));

        // Verify cluster and operation time.
        if (shouldAdvance) {
            assert(bsonWoCompare(mongo.getOperationTime(), operationTime) > 0,
                   "expected the shell's operationTime to increase after running command: " +
                       tojson(cmdObj));
            assert(
                bsonWoCompare(mongo.getClusterTime().clusterTime, clusterTimeObj.clusterTime) > 0,
                "expected the shell's clusterTime value to increase after running command: " +
                    tojson(cmdObj));
        } else {
            assert(bsonWoCompare(mongo.getOperationTime(), operationTime) == 0,
                   "expected the shell's operationTime to not change after running command: " +
                       tojson(cmdObj));
            // Don't check clusterTime, because during a slow operation clusterTime may be
            // incremented by unrelated activity in the cluster.
        }
    }

    // Verifies the command works and its response satisfies the callback.
    function commandReturnsExpectedResult(cmdObj, db, resCallback) {
        const mongo = db.getMongo();

        // Use the latest cluster time returned as a new operationTime and run command.
        const clusterTimeObj = mongo.getClusterTime();
        mongo.setOperationTime(clusterTimeObj.clusterTime);
        const res = assert.commandWorked(testDB.runCommand(cmdObj));

        // Verify the response contents and that new operation time is >= passed in time.
        assert(bsonWoCompare(mongo.getOperationTime(), clusterTimeObj.clusterTime) >= 0,
               "expected the shell's operationTime to be >= to:" + clusterTimeObj.clusterTime +
                   " after running command: " + tojson(cmdObj));
        resCallback(res);
    }

    // All commands currently enabled to use causal consistency in the shell.
    const supportedCommandNames = [
        {"query": {"aggregate": "test", "pipeline": [{"$match": {"x": 1}}]}},
        {"aggregate": "test", "pipeline": [{"$match": {"x": 1}}]},
        {"group": {"key": {"x": 1}}},
        {"query": {"group": {"key": {"x": 1}}}},
        {"query": {"explain": {"group": {"key": {"x": 1}}}}},
        {"count": "test", "query": {}},
        {"query": {"count": "test", "query": {}}},
        {"query": {"explain": {"count": "test", "query": {}}}},
        {"explain": {"count": "test", "query": {}}},
        {"distinct": "test", "query": {}},
        {"query": {"distinct": "test", "query": {}}},
        {"query": {"explain": {"distinct": "test", "query": {}}}},
        {"find": "test", "query": {}},
        {"query": {"find": "test", "query": {}}},
        {"query": {"explain": {"find": "test", "query": {}}}},
        {"geoNear": "test", "near": {}},
        {"query": {"geoNear": "test", "near": {}}},
        {"query": {"explain": {"geoNear": "test", "near": {}}}},
        {"geoSearch": "test", "near": {}},
        {"mapReduce": "test"},
        {"parallelCollectionScan": "test"},
        {"getMore": NumberLong("5888577173997830861")}
    ];

    // Omitting some commands for simplicity. Every command not listed above should be unsupported.
    const unsupportedCommandNames = [
        {"aggregate": "test", "pipeline": [{"$match": {"x": 1}}], "explain": true},
        {"explain": {"aggregate": "test", "pipeline": [{"$match": {"x": 1}}]}},
        {"delete": "coll", "query": {"x": 1}},
        {"explain": {"delete": "coll", "query": {"x": 1}}},
        {"findAndModify": "coll", "query": {"x": 1}},
        {"explain": {"findAndModify": "coll", "query": {"x": 1}}},
        {"query": {"explain": {"findAndModify": "coll", "query": {"x": 1}}}},
        {"insert": "coll"},
        {"explain": {"insert": "coll"}},
        {"explain": {"update": "coll"}},
        {"update": "coll"},
        {"getLastError": {"x": 1}},
        {"getPrevError": {"x": 1}}
    ];

    // Manually create a shard so tests on storage engines that don't support majority readConcern
    // can exit early.
    const rsName = "causal_consistency_shell_support_rs";
    const rst = new ReplSetTest({
        nodes: 1,
        name: rsName,
        nodeOptions: {
            enableMajorityReadConcern: "",
            shardsvr: "",
        }
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }
    rst.initiate();

    // Start the sharding test and add the majority readConcern enabled replica set.
    const name = "causal_consistency_shell_support";
    const st =
        new ShardingTest({name: name, shards: 1, manualAddShard: true, mongosWaitsForKeys: true});
    assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));

    const testDB = st.s.getDB("test");
    const mongo = testDB.getMongo();

    // Verify causal consistency is disabled unless explicitly set.
    assert.eq(!!mongo._isCausal, false);
    mongo.setCausalConsistency(true);

    // Verify causal consistency is enabled for the connection and for each supported command.
    assert.eq(!!mongo._isCausal, true);
    checkCausalConsistencySupportForCommandNames(supportedCommandNames, true);
    checkCausalConsistencySupportForCommandNames(unsupportedCommandNames, false);

    // Verify cluster times are tracked even before causal consistency is set (so the first
    // operation with causal consistency set can use valid cluster times).
    mongo._operationTime = null;
    mongo._clusterTime = null;

    assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 1}]}));
    assert.neq(mongo.getOperationTime(), null);
    assert.neq(mongo.getClusterTime(), null);

    mongo._operationTime = null;
    mongo._clusterTime = null;

    assert.commandWorked(testDB.runCommand({find: "foo"}));
    assert.neq(mongo.getOperationTime(), null);
    assert.neq(mongo.getClusterTime(), null);

    // Test that write commands advance both operation and cluster time.
    runCommandAndCheckLogicalTimes({insert: "foo", documents: [{x: 2}]}, testDB, true);
    runCommandAndCheckLogicalTimes(
        {update: "foo", updates: [{q: {x: 2}, u: {$set: {x: 3}}}]}, testDB, true);

    // Test that each supported command works as expected and the shell's cluster times are properly
    // forwarded to the server and updated based on the response.
    mongo.setCausalConsistency(true);

    // Aggregate command.
    let aggColl = "aggColl";
    let aggCmd = {aggregate: aggColl, pipeline: [{$match: {x: 1}}], cursor: {}};
    let aggCallback = function(res) {
        assert.eq(res.cursor.firstBatch, [{_id: 1, x: 1}]);
    };

    runCommandAndCheckLogicalTimes({insert: aggColl, documents: [{_id: 1, x: 1}]}, testDB, true);
    runCommandAndCheckLogicalTimes(aggCmd, testDB, false);
    commandReturnsExpectedResult(aggCmd, testDB, aggCallback);

    // Count command.
    let countColl = "countColl";
    let countCmd = {count: countColl};
    let countCallback = function(res) {
        assert.eq(res.n, 1);
    };

    runCommandAndCheckLogicalTimes({insert: countColl, documents: [{_id: 1, x: 1}]}, testDB, true);
    runCommandAndCheckLogicalTimes(countCmd, testDB, false);
    commandReturnsExpectedResult(countCmd, testDB, countCallback);

    // Distinct command.
    let distinctColl = "distinctColl";
    let distinctCmd = {distinct: distinctColl, key: "x"};
    let distinctCallback = function(res) {
        assert.eq(res.values, [1]);
    };

    runCommandAndCheckLogicalTimes(
        {insert: distinctColl, documents: [{_id: 1, x: 1}]}, testDB, true);
    runCommandAndCheckLogicalTimes(distinctCmd, testDB, false);
    commandReturnsExpectedResult(distinctCmd, testDB, distinctCallback);

    // Find command.
    let findColl = "findColl";
    let findCmd = {find: findColl};
    let findCallback = function(res) {
        assert.eq(res.cursor.firstBatch, [{_id: 1, x: 1}]);
    };

    runCommandAndCheckLogicalTimes({insert: findColl, documents: [{_id: 1, x: 1}]}, testDB, true);
    runCommandAndCheckLogicalTimes(findCmd, testDB, false);
    commandReturnsExpectedResult(findCmd, testDB, findCallback);

    // GeoNear command.
    let geoNearColl = "geoNearColl";
    let geoNearCmd = {
        geoNear: geoNearColl,
        near: {type: "Point", coordinates: [-10, 10]},
        spherical: true
    };
    let geoNearCallback = function(res) {
        assert.eq(res.results[0].obj, {_id: 1, loc: {type: "Point", coordinates: [-10, 10]}});
    };

    assert.commandWorked(testDB[geoNearColl].createIndex({loc: "2dsphere"}));
    runCommandAndCheckLogicalTimes(
        {insert: geoNearColl, documents: [{_id: 1, loc: {type: "Point", coordinates: [-10, 10]}}]},
        testDB,
        true);
    runCommandAndCheckLogicalTimes(geoNearCmd, testDB, false);
    commandReturnsExpectedResult(geoNearCmd, testDB, geoNearCallback);

    // GeoSearch is not supported for sharded clusters.

    // MapReduce doesn't currently support read concern majority.

    // ParallelCollectionScan is not available on mongos.

    // Verify that the server rejects commands when operation time is invalid by running a command
    // with an afterClusterTime value one day ahead.
    const invalidTime = new Timestamp(mongo.getOperationTime().getTime() + (60 * 60 * 24), 0);
    const invalidCmd = {
        find: "foo",
        readConcern: {level: "majority", afterClusterTime: invalidTime}
    };
    assert.commandFailedWithCode(
        testDB.runCommand(invalidCmd),
        ErrorCodes.InvalidOptions,
        "expected command, " + tojson(invalidCmd) + ", to fail with code, " +
            ErrorCodes.InvalidOptions + ", because the afterClusterTime value, " + invalidTime +
            ", should not be ahead of the clusterTime, " + mongo.getClusterTime().clusterTime);

    st.stop();
})();
