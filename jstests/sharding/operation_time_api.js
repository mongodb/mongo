/**
 * Tests the operationTime API for the following topologies:
 *   - merizos talking to a sharded replica set (sharded and unsharded collections)
 *   - merizod from a sharded replica set
 *   - merizod from a non-sharded replica set
 *   - standalone merizod
 */
(function() {
    "use strict";

    function responseContainsTimestampOperationTime(res) {
        return res.operationTime !== undefined && isTimestamp(res.operationTime);
    }

    function isTimestamp(val) {
        return Object.prototype.toString.call(val) === "[object Timestamp]";
    }

    // A merizos that talks to a non-sharded collection on a sharded replica set returns an
    // operationTime that is a Timestamp.
    var st = new ShardingTest(
        {name: "operation_time_api", shards: {rs0: {nodes: 1}}, merizosWaitsForKeys: true});

    var testDB = st.s.getDB("test");
    var res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 1}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a merizos talking to a non-sharded collection on a sharded " +
               "replica set to contain an operationTime, received: " + tojson(res));

    // A merizos that talks to a sharded collection on a sharded replica set returns an operationTime
    // that is a Timestamp.
    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: "test.bar", key: {x: 1}}));

    res = assert.commandWorked(testDB.runCommand({insert: "bar", documents: [{x: 2}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a merizos inserting to a sharded collection on a sharded " +
               "replica set to contain an operationTime, received: " + tojson(res));

    // A merizod in a sharded replica set returns an operationTime that is a Timestamp.
    testDB = st.rs0.getPrimary().getDB("test");
    res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 3}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a merizod in a sharded replica set to contain an " +
               "operationTime, received: " + tojson(res));

    st.stop();

    // A merizod from a non-sharded replica set returns an operationTime that is a Timestamp.
    var replTest = new ReplSetTest({name: "operation_time_api_non_sharded_replset", nodes: 1});
    replTest.startSet();
    replTest.initiate();

    testDB = replTest.getPrimary().getDB("test");
    res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 4}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a non-sharded replica set to contain an operationTime, " +
               "received: " + tojson(res));

    replTest.stopSet();

    // A standalone merizod does not return an operationTime.
    var standalone = MongoRunner.runMongod();

    testDB = standalone.getDB("test");
    res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 5}]}));
    assert(!responseContainsTimestampOperationTime(res),
           "Expected response from a standalone merizod to not contain an operationTime, " +
               "received: " + tojson(res));

    MongoRunner.stopMongod(standalone);
})();
