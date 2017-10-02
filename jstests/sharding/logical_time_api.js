/**
 * Tests the logicalTime API for the following topologies:
 *   - mongos talking to a sharded replica set (sharded and unsharded collections)
 *   - mongod from a sharded replica set
 *   - mongod from a non-sharded replica set
 *   - standalone mongod
 *
 * Expects logicalTime to come in the command body from both a mongos and a mongod.
 */
(function() {
    "use strict";

    // Returns true if the given object contains a logicalTime BSON object in the following format:
    // $clusterTime: {
    //     clusterTime: <Timestamp>
    //     signature: {
    //         hash: <BinData>
    //         keyId: <NumberLong>
    //     }
    // }
    function containsValidLogicalTimeBson(obj) {
        if (!obj) {
            return false;
        }

        var logicalTime = obj.$clusterTime;
        return logicalTime && isType(logicalTime, "BSON") &&
            isType(logicalTime.clusterTime, "Timestamp") && isType(logicalTime.signature, "BSON") &&
            isType(logicalTime.signature.hash, "BinData") &&
            isType(logicalTime.signature.keyId, "NumberLong");
    }

    function isType(val, typeString) {
        assert.eq(Object.prototype.toString.call(val),
                  "[object " + typeString + "]",
                  "expected: " + val + ", to be of type: " + typeString);
        return true;
    }

    // A mongos that talks to a non-sharded collection on a sharded replica set returns a
    // logicalTime BSON object that matches the expected format.
    var st = new ShardingTest(
        {name: "logical_time_api", shards: {rs0: {nodes: 1}}, mongosWaitsForKeys: true});

    var testDB = st.s.getDB("test");
    var res =
        assert.commandWorked(testDB.runCommand("insert", {insert: "foo", documents: [{x: 1}]}));
    assert(containsValidLogicalTimeBson(res),
           "Expected command body from a mongos talking to a non-sharded collection on a sharded " +
               "replica set to contain logicalTime, received: " + tojson(res));

    // A mongos that talks to a sharded collection on a sharded replica set returns a
    // logicalTime BSON object that matches the expected format.
    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: "test.bar", key: {x: 1}}));

    res = assert.commandWorked(testDB.runCommand("insert", {insert: "bar", documents: [{x: 2}]}));
    assert(containsValidLogicalTimeBson(res),
           "Expected command body from a mongos talking to a sharded collection on a sharded " +
               "replica set to contain logicalTime, received: " + tojson(res));

    // Verify mongos can accept requests with $clusterTime in the command body.
    assert.commandWorked(testDB.runCommand({isMaster: 1, $clusterTime: res.$clusterTime}));

    // A mongod in a sharded replica set returns a logicalTime bson that matches the expected
    // format.
    testDB = st.rs0.getPrimary().getDB("test");
    res = assert.commandWorked(testDB.runCommand("insert", {insert: "foo", documents: [{x: 3}]}));
    assert(containsValidLogicalTimeBson(res),
           "Expected command body from a mongod in a sharded replica set to contain " +
               "logicalTime, received: " + tojson(res));

    // Verify mongod can accept requests with $clusterTime in the command body.
    res = assert.commandWorked(testDB.runCommand({isMaster: 1, $clusterTime: res.$clusterTime}));

    st.stop();

    // A mongod from a non-sharded replica set does not return logicalTime.
    var replTest = new ReplSetTest({name: "logical_time_api_non_sharded_replset", nodes: 1});
    replTest.startSet();
    replTest.initiate();

    testDB = replTest.getPrimary().getDB("test");
    res = assert.commandWorked(testDB.runCommand("insert", {insert: "foo", documents: [{x: 4}]}));
    assert(containsValidLogicalTimeBson(res),
           "Expected command body from a mongod in a non-sharded replica set to " +
               "contain logicalTime, received: " + tojson(res));

    replTest.stopSet();

    // A standalone mongod does not return logicalTime.
    var standalone = MongoRunner.runMongod();

    testDB = standalone.getDB("test");
    res = assert.commandWorked(testDB.runCommand("insert", {insert: "foo", documents: [{x: 5}]}));
    assert(!containsValidLogicalTimeBson(res),
           "Expected command body from a standalone mongod to not contain logicalTime, " +
               "received: " + tojson(res));

    MongoRunner.stopMongod(standalone);
})();
