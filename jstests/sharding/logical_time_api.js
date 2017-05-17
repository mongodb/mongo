/**
 * Tests the logicalTime API for the following topologies:
 *   - mongos talking to a sharded replica set (sharded and unsharded collections)
 *   - mongod from a sharded replica set
 *   - mongod from a non-sharded replica set
 *   - standalone mongod
 *
 * Expects logicalTime to come in the commandReply from a mongos and the metadata from a mongod.
 */
(function() {
    "use strict";

    // Returns true if the given object contains a logicalTime BSON object in the following format:
    // $logicalTime: {
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

        var logicalTime = obj.$logicalTime;
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
    var st = new ShardingTest({name: "logical_time_api", shards: {rs0: {nodes: 1}}});

    var testDB = st.s.getDB("test");
    var res = testDB.runCommandWithMetadata("insert", {insert: "foo", documents: [{x: 1}]}, {});
    assert.commandWorked(res.commandReply);
    assert(containsValidLogicalTimeBson(res.commandReply),
           "Expected commandReply from a mongos talking to a non-sharded collection on a sharded " +
               "replica set to contain logicalTime, received: " + tojson(res.commandReply));

    // A mongos that talks to a sharded collection on a sharded replica set returns a
    // logicalTime BSON object that matches the expected format.
    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: "test.bar", key: {x: 1}}));

    res = testDB.runCommandWithMetadata("insert", {insert: "bar", documents: [{x: 2}]}, {});
    assert.commandWorked(res.commandReply);
    assert(containsValidLogicalTimeBson(res.commandReply),
           "Expected commandReply from a mongos talking to a sharded collection on a sharded " +
               "replica set to contain logicalTime, received: " + tojson(res.commandReply));

    // A mongod in a sharded replica set returns a logicalTime bson that matches the expected
    // format.
    testDB = st.rs0.getPrimary().getDB("test");
    res = testDB.runCommandWithMetadata("insert", {insert: "foo", documents: [{x: 3}]}, {});
    assert.commandWorked(res.commandReply);
    assert(containsValidLogicalTimeBson(res.metadata),
           "Expected metadata in response from a mongod in a sharded replica set to contain " +
               "logicalTime, received: " + tojson(res.metadata));

    st.stop();

    // A mongod from a non-sharded replica set does not return logicalTime.
    var replTest = new ReplSetTest({name: "logical_time_api_non_sharded_replset", nodes: 1});
    replTest.startSet();
    replTest.initiate();

    testDB = replTest.getPrimary().getDB("test");
    res = testDB.runCommandWithMetadata("insert", {insert: "foo", documents: [{x: 4}]}, {});
    assert.commandWorked(res.commandReply);
    assert(!containsValidLogicalTimeBson(res.metadata),
           "Expected metadata in response from a mongod in a non-sharded replica set to not " +
               "contain logicalTime, received: " + tojson(res.metadata));

    replTest.stopSet();

    // A standalone mongod does not return logicalTime.
    var standalone = MongoRunner.runMongod();

    testDB = standalone.getDB("test");
    res = testDB.runCommandWithMetadata("insert", {insert: "foo", documents: [{x: 5}]}, {});
    assert.commandWorked(res.commandReply);
    assert(!containsValidLogicalTimeBson(res.metadata),
           "Expected metadata in response from a standalone mongod to not contain logicalTime, " +
               "received: " + tojson(res.metadata));

    MongoRunner.stopMongod(standalone);
})();
