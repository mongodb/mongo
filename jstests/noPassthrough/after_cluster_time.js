// This test verifies readConcern:afterClusterTime behavior on a standalone mongod.
(function() {
    "use strict";
    var standalone =
        MongoRunner.runMongod({enableMajorityReadConcern: "", storageEngine: "wiredTiger"});

    var testDB = standalone.getDB("test");
    const res = assert.commandWorked(testDB.runCommand({
        find: "after_cluster_time",
        readConcern: {level: "majority", afterClusterTime: Timestamp(0, 0)}
    }));

    MongoRunner.stopMongod(standalone);
}());
