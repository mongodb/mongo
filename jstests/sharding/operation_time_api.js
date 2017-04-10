/**
 * Tests the operationTime API for the following topologies:
 *   - mongos talking to a sharded replica set (sharded and unsharded collections)
 *   - mongod from a sharded replica set
 *   - mongod from a non-sharded replica set
 *   - standalone mongod
 */
(function() {
    function responseContainsTimestampOperationTime(res) {
        return res.operationTime !== undefined && isTimestamp(res.operationTime);
    }

    function isTimestamp(val) {
        return Object.prototype.toString.call(val) === "[object Timestamp]";
    }

    // A mongos that talks to a non-sharded collection on a sharded replica set returns an
    // operationTime that is a Timestamp.
    var name = "operation_time_api";
    var st = new ShardingTest({name: name, shards: {rs0: {nodes: 1}}});

    var testDB = st.s.getDB("test");
    res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 1}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a mongos talking to a non-sharded collection on a sharded " +
               "replica set to contain an operationTime.");

    // A mongos that talks to a sharded collection on a sharded replica set returns an operationTime
    // that is a Timestamp.
    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: "test.bar", key: {x: 1}}));

    res = assert.commandWorked(testDB.runCommand({insert: "bar", documents: [{x: 2}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a mongos inserting to a sharded collection on a sharded " +
               "replica set to contain an operationTime.");

    // A mongod in a sharded replica set returns an operationTime that is a Timestamp.
    testDB = st.rs0.getPrimary().getDB("test");
    res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 3}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a mongod in a sharded replica set to contain an operationTime");

    st.stop();

    // A mongod from a non-sharded replica set returns an operationTime that is a Timestamp.
    var replSetName = "operation_time_api_non_sharded_replset";
    var replTest = new ReplSetTest({name: replSetName, nodes: 1});
    replTest.startSet();
    replTest.initiate();

    testDB = replTest.getPrimary().getDB("test");
    res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 4}]}));
    assert(responseContainsTimestampOperationTime(res),
           "Expected response from a non-sharded replica set to contain an operationTime.");

    replTest.stopSet();

    // A standalone mongod does not return an operationTime.
    var standalone = MongoRunner.runMongod();

    testDB = standalone.getDB("test");
    res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 5}]}));
    assert(!responseContainsTimestampOperationTime(res),
           "Expected response from a standalone mongod to not contain an operationTime.");

    MongoRunner.stopMongod(standalone);
})();
