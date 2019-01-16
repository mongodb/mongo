// Tests readConcern level metrics in the serverStatus output.
// @tags: [requires_majority_read_concern, requires_wiredtiger]
(function() {
    "use strict";

    // Verifies that the server status response has the fields that we expect.
    function verifyServerStatusFields(serverStatusResponse) {
        assert(serverStatusResponse.hasOwnProperty("opReadConcernCounters"),
               "Expected the serverStatus response to have a 'opReadConcernCounters' field\n" +
                   tojson(serverStatusResponse));
        assert(
            serverStatusResponse.opReadConcernCounters.hasOwnProperty("available"),
            "The 'opReadConcernCounters' field in serverStatus did not have the 'available' field\n" +
                tojson(serverStatusResponse.opReadConcernCounters));
        assert(
            serverStatusResponse.opReadConcernCounters.hasOwnProperty("linearizable"),
            "The 'opReadConcernCounters' field in serverStatus did not have the 'linearizable' field\n" +
                tojson(serverStatusResponse.opReadConcernCounters));
        assert(
            serverStatusResponse.opReadConcernCounters.hasOwnProperty("local"),
            "The 'opReadConcernCounters' field in serverStatus did not have the 'local' field\n" +
                tojson(serverStatusResponse.opReadConcernCounters));
        assert(
            serverStatusResponse.opReadConcernCounters.hasOwnProperty("majority"),
            "The 'opReadConcernCounters' field in serverStatus did not have the 'majority' field\n" +
                tojson(serverStatusResponse.opReadConcernCounters));
        assert(serverStatusResponse.opReadConcernCounters.hasOwnProperty("none"),
               "The 'opReadConcernCounters' field in serverStatus did not have the 'none' field\n" +
                   tojson(serverStatusResponse.opReadConcernCounters));
    }

    // Verifies that the given value of the server status response is incremented in the way
    // we expect.
    function verifyServerStatusChange(initialStats, newStats, valueName, expectedIncrement) {
        assert.eq(initialStats[valueName] + expectedIncrement,
                  newStats[valueName],
                  "expected " + valueName + " to increase by " + expectedIncrement +
                      ", initialStats: " + tojson(initialStats) + ", newStats: " +
                      tojson(newStats));
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const dbName = "test";
    const collName = "server_read_concern_metrics";
    const testDB = primary.getDB(dbName);
    const testColl = testDB[collName];
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.writeOK(testColl.insert({_id: 0}));

    // Get initial serverStatus.
    let serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);

    // Run a find with no readConcern.
    assert.eq(testColl.find().itcount(), 1);
    let newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 1);
    serverStatus = newStatus;

    // Run a find with a readConcern with no level.
    assert.commandWorked(
        testDB.runCommand({find: collName, readConcern: {afterClusterTime: Timestamp(1, 1)}}));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 1);
    serverStatus = newStatus;

    // Run a legacy query.
    primary.forceReadMode("legacy");
    assert.eq(testColl.find().itcount(), 1);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 1);
    primary.forceReadMode("commands");
    serverStatus = newStatus;

    // Run a find with a readConcern level available.
    assert.eq(testColl.find().readConcern("available").itcount(), 1);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 1);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 0);
    serverStatus = newStatus;

    // Run a find with a readConcern level linearizable.
    assert.eq(testColl.find().readConcern("linearizable").itcount(), 1);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 1);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 0);
    serverStatus = newStatus;

    // Run a find with a readConcern level local.
    assert.eq(testColl.find().readConcern("local").itcount(), 1);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 1);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 0);
    serverStatus = newStatus;

    // Run a find with a readConcern level majority.
    assert.eq(testColl.find().readConcern("majority").itcount(), 1);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 1);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 0);
    serverStatus = newStatus;

    // Aggregation does not count toward readConcern metrics. Aggregation is counted as a 'command'
    // in the 'opCounters' serverStatus section, and we only track the readConcern of queries
    // tracked in 'opCounters.query'.
    assert.eq(testColl.aggregate([]).itcount(), 1);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 0);
    serverStatus = newStatus;

    // The count command does not count toward readConcern metrics. The count command is counted as
    // a 'command' in the 'opCounters' serverStatus section, and we only track the readConcern of
    // queries tracked in 'opCounters.query'.
    assert.eq(testColl.count({_id: 0}), 1);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 0);
    serverStatus = newStatus;

    // getMore does not count toward readConcern metrics. getMore inherits the readConcern of the
    // originating command. It is not counted in 'opCounters.query'.
    let res = assert.commandWorked(testDB.runCommand({find: collName, batchSize: 0}));
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.commandWorked(testDB.runCommand({getMore: res.cursor.id, collection: collName}));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "available", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "linearizable", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "local", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "majority", 0);
    verifyServerStatusChange(
        serverStatus.opReadConcernCounters, newStatus.opReadConcernCounters, "none", 0);

    rst.stopSet();
}());
