// This test verifies readConcern:afterClusterTime behavior on a standalone mongod.
(function() {
    "use strict";
    var standalone =
        MongoRunner.runMongod({enableMajorityReadConcern: "", storageEngine: "wiredTiger"});

    var testDB = standalone.getDB("test");

    assert.commandWorked(testDB.runCommand({insert: "after_cluster_time", documents: [{x: 1}]}));

    // Majority reads without afterClusterTime succeed.
    assert.commandWorked(
        testDB.runCommand({find: "after_cluster_time", readConcern: {level: "majority"}}),
        "expected majority read without afterClusterTime to succeed on standalone mongod");

    // afterClusterTime reads without a level fail.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {find: "after_cluster_time", readConcern: {afterClusterTime: Timestamp(0, 0)}}),
        ErrorCodes.InvalidOptions,
        "expected non-majority afterClusterTime read to fail on standalone mongod");

    // afterClusterTime reads with null timestamps are rejected.
    assert.commandFailedWithCode(
        testDB.runCommand({
            find: "after_cluster_time",
            readConcern: {level: "majority", afterClusterTime: Timestamp(0, 0)}
        }),
        ErrorCodes.InvalidOptions,
        "expected afterClusterTime read with null timestamp to fail on standalone mongod");

    // Standalones don't store clusterTime, so any non-zero afterClusterTime read value will be
    // rejected for being ahead of the server's uninitialized internal clusterTime.
    assert.commandFailedWithCode(
        testDB.runCommand({
            find: "after_cluster_time",
            readConcern: {level: "majority", afterClusterTime: Timestamp(0, 1)}
        }),
        ErrorCodes.InvalidOptions,
        "expected afterClusterTime read with non-zero timestamp to fail on standalone mongod");

    MongoRunner.stopMongod(standalone);
}());
